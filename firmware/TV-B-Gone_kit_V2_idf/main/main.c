/*
  TV-B-Gone V2 — ESP32-C3 Super Mini board
  ESP-IDF v5.5, plain C, FreeRTOS task

  Hardware connections:
  - 3xAA battery pack between "5V" and "GND" (Vcc = 4.5V)
  - IR LED on GPIO2 through IRLU024NPBF MOSFET amplifier
  - NA pushbutton between GPIO10 and GND (internal pull-up)
  - EU pushbutton between GPIO9  and GND (internal pull-up)
  - Visible LED: built-in LED on GPIO8 (active-LOW)

  app_main() initialises hardware, creates tvb_gone_task(), then returns.
  All transmission logic runs inside the task — non-blocking for the scheduler.

  Note on timing: xmitPair() uses esp_rom_delay_us() for microsecond-accurate
  IR pulses. FreeRTOS cannot schedule at µs resolution, so busy-waiting at that
  level is intentional. Inter-code gaps use vTaskDelay() to yield to the RTOS.

  Creative Commons CC BY-SA 4.0
*/

#include "main.h"

#include <inttypes.h>           /* PRIu32 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "TV-B-Gone";

/* GPIO level aliases */
#define LOW  0
#define HIGH 1

/* Timing constants (milliseconds) */
#define VIS_LED_BLIP_TIME    25   /* LED blip before each POWER-Code */
#define TIME_BETWEEN_CODES  205   /* gap between POWER-Codes */
#define BLIP_TIME           125   /* delay between indication blinks */

/* LEDC / PWM constants */
#define DUTY_RESOLUTION       8   /* 8-bit → duty range 0–255 */
#define DUTY_CYCLE          128   /* 50% duty cycle */
#define PWM_CHANNEL           0   /* LEDC channel */

/* Power-code arrays defined in WORLDcodes.cpp (extern "C" there → plain C here) */
extern struct IrCode *NApowerCodes[];
extern struct IrCode *EUpowerCodes[];
extern uint8_t num_NAcodes;
extern uint8_t num_EUcodes;

/* Forward declarations */
static void xmitPair(uint32_t carFreq, uint32_t onTime, uint32_t offTime);
static void blinkLEDnTimes(int numBlinks);
static void tvb_gone_task(void *pvParameters);


/* -----------------------------------------------------------------------
 * xmitPair()
 *
 * Transmit one On-Time/Off-Time IR pulse pair.
 *
 * If carFreq != 0: pulse the IR LED at carFreq Hz for onTime µs,
 *                  then keep it off for offTime µs.
 * If carFreq == 0: hold IR LED solidly ON for onTime µs (no carrier),
 *                  then off for offTime µs.
 *
 * LEDC is initialised once in app_main(); only frequency and duty are
 * updated here, avoiding repeated GPIO reservation.
 * ----------------------------------------------------------------------- */
static void xmitPair(uint32_t carFreq, uint32_t onTime, uint32_t offTime)
{
    DEBUGP(ESP_LOGI(TAG, "%" PRIu32 ", %" PRIu32, onTime, offTime));

    if (carFreq != 0) {
        /* Update carrier frequency then enable PWM output */
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, carFreq);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)PWM_CHANNEL, DUTY_CYCLE);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)PWM_CHANNEL);
    } else {
        /* No carrier — hold IR LED solidly ON via LEDC idle level */
        ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)PWM_CHANNEL, 1);
    }

    esp_rom_delay_us(onTime);   /* ON period — busy-wait required for µs accuracy */

    /* IR LED OFF: stop PWM, hold output LOW */
    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)PWM_CHANNEL, 0);
    esp_rom_delay_us(offTime);  /* OFF period */
}


/* -----------------------------------------------------------------------
 * blinkLEDnTimes()
 *
 * Quickly blink the visible LED numBlinks times.
 * The built-in LED on the ESP32-C3 Super Mini is active-LOW.
 * ----------------------------------------------------------------------- */
static void blinkLEDnTimes(int numBlinks)
{
    for (int i = 0; i < numBlinks; i++) {
        gpio_set_level((gpio_num_t)VISLED, LOW);   /* LED ON  */
        vTaskDelay(pdMS_TO_TICKS(VIS_LED_BLIP_TIME));
        gpio_set_level((gpio_num_t)VISLED, HIGH);  /* LED OFF */
        vTaskDelay(pdMS_TO_TICKS(BLIP_TIME));
    }
}


/* -----------------------------------------------------------------------
 * tvb_gone_task()
 *
 * Main application logic, running as a FreeRTOS task so app_main() can
 * return immediately after xTaskCreate().
 *
 * Behaviour:
 *   - Polls NA / EU buttons each outer loop iteration.
 *   - When a button is pressed, transmits the full regional database.
 *   - If a button is pressed mid-transmission, restarts from code 0.
 *   - While idle (no button): yields to the scheduler every 10 ms.
 *
 * The goto-based restart from the original loop() is replaced by a
 * do { } while (startOver) inner loop — same semantics, no goto.
 * ----------------------------------------------------------------------- */
static void tvb_gone_task(void *pvParameters)
{
    int buttonStateNA;
    int buttonStateEU;
    int buttonStateNA_again;
    int buttonStateEU_again;
    int region = NA;
    int startOver = FALSE;
    int numCodes = 0;
    int numPairs;
    uint32_t carrierFreq;
    uint8_t  pairsIndex;
    uint32_t OnTime;
    uint32_t OffTime;
    struct IrCode *pwrCode_ptr;
    uint32_t      *pairsTab_ptr;
    uint8_t       *sequenceTab_ptr;

    for (;;) {

        /* Read button states at the start of each idle poll cycle */
        buttonStateNA = gpio_get_level((gpio_num_t)BUTTON_NA);
        buttonStateEU = gpio_get_level((gpio_num_t)BUTTON_EU);

        /* Inner loop: handles the "restart from beginning" case without goto */
        do {
            if (startOver) {
                /* Button was pressed during transmission — restart with new state */
                buttonStateNA = buttonStateNA_again;
                buttonStateEU = buttonStateEU_again;
                startOver = FALSE;
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "Transmission restarting from the beginning");
                ESP_LOGI(TAG, "");
            }

            if ((!buttonStateNA || !buttonStateEU) == HIGH) {

                vTaskDelay(pdMS_TO_TICKS(250));  /* debounce / settle */

                if (buttonStateNA == LOW) {
                    region   = NA;
                    numCodes = num_NAcodes;
                    blinkLEDnTimes(3);
                    ESP_LOGI(TAG, "NA button pressed");
                    ESP_LOGI(TAG, "# of NA POWER-Codes: %d", numCodes);
                    ESP_LOGI(TAG, "");
                } else if (buttonStateEU == LOW) {
                    region   = EU;
                    numCodes = num_EUcodes;
                    blinkLEDnTimes(6);
                    ESP_LOGI(TAG, "EU button pressed");
                    ESP_LOGI(TAG, "# of EU POWER-Codes: %d", numCodes);
                    ESP_LOGI(TAG, "");
                } else {
                    blinkLEDnTimes(25);
                    DEBUGP(ESP_LOGI(TAG, "ERROR: no button pressed, yet transmission started"));
                }

                vTaskDelay(pdMS_TO_TICKS(500));  /* pause before first code */

                /* --- Transmit all POWER-Codes for the selected region --- */
                for (int powerCodeCount = 0; powerCodeCount < numCodes; powerCodeCount++) {

                    pwrCode_ptr = (region == NA) ? NApowerCodes[powerCodeCount]
                                                 : EUpowerCodes[powerCodeCount];

                    /* Blip the visible LED before each code */
                    gpio_set_level((gpio_num_t)VISLED, LOW);
                    vTaskDelay(pdMS_TO_TICKS(VIS_LED_BLIP_TIME));
                    gpio_set_level((gpio_num_t)VISLED, HIGH);

                    carrierFreq    = pwrCode_ptr->carrier_freq;
                    numPairs       = pwrCode_ptr->num_pairs;
                    pairsTab_ptr   = pwrCode_ptr->pairs;
                    sequenceTab_ptr = pwrCode_ptr->sequence;

#if DEBUG > 0
                    DEBUGP(ESP_LOGI(TAG, "Carrier Frequency: %" PRIu32, carrierFreq));
                    DEBUGP(ESP_LOGI(TAG, "Num pairs: %d", numPairs));
                    if (DEBUG == 1) {
                        ESP_LOGI(TAG, "First 4 unique pairs:");
                        for (int i = 0; i < 8; i += 2) {
                            ESP_LOGI(TAG, "  %" PRIu32 ",  %" PRIu32,
                                     pairsTab_ptr[i], pairsTab_ptr[i + 1]);
                        }
                        ESP_LOGI(TAG, "Sequence:");
                        for (int i = 0; i < numPairs; i++) {
                            ESP_LOGI(TAG, "  %d,", sequenceTab_ptr[i]);
                        }
                    }
#endif

                    ESP_LOGI(TAG, "Transmitting POWER-Code %d...", powerCodeCount);

                    for (int i = 0; i < numPairs; i++) {
                        pairsIndex = sequenceTab_ptr[i];
                        DEBUGP(ESP_LOGI(TAG, " %d: ", pairsIndex));
                        pairsIndex = pairsIndex * 2;
                        OnTime  = pairsTab_ptr[pairsIndex];
                        OffTime = pairsTab_ptr[pairsIndex + 1];
                        xmitPair(carrierFreq, OnTime, OffTime);
                    }

                    ESP_LOGI(TAG, "Done");

                    /* Inter-code gap (subtract the blip time already spent) */
                    vTaskDelay(pdMS_TO_TICKS(TIME_BETWEEN_CODES - VIS_LED_BLIP_TIME));

                    /* Check for restart request */
                    buttonStateNA_again = gpio_get_level((gpio_num_t)BUTTON_NA);
                    buttonStateEU_again = gpio_get_level((gpio_num_t)BUTTON_EU);
                    if ((!buttonStateNA_again || !buttonStateEU_again) == HIGH) {
                        startOver = TRUE;
                        break;
                    }
                }

                if (!startOver) {
                    /* Finished the entire database */
                    ESP_LOGI(TAG, "");
                    if (buttonStateNA == LOW) {
                        ESP_LOGI(TAG, "Finished transmitting entire database for NA.");
                    } else if (buttonStateEU == LOW) {
                        ESP_LOGI(TAG, "Finished transmitting entire database for EU.");
                    } else {
                        DEBUGP(ESP_LOGI(TAG, "Finished transmitting entire database for ERROR."));
                    }
                    ESP_LOGI(TAG, "");

                    vTaskDelay(pdMS_TO_TICKS(500));
                    if (buttonStateNA == LOW) {
                        blinkLEDnTimes(3);
                    } else if (buttonStateEU == LOW) {
                        blinkLEDnTimes(6);
                    } else {
                        blinkLEDnTimes(25);
                    }
                    vTaskDelay(pdMS_TO_TICKS(500));
                }

            } else {
                /* No button pressed — keep outputs safe and yield */
                ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)PWM_CHANNEL, 0); /* IR LED off */
                gpio_set_level((gpio_num_t)VISLED, HIGH);                        /* VIS LED off */
                vTaskDelay(pdMS_TO_TICKS(10));
            }

        } while (startOver);

    } /* for(;;) */
}


/* -----------------------------------------------------------------------
 * app_main()
 *
 * ESP-IDF entry point.  Initialises hardware once, creates the task,
 * then returns — the task runs independently on the FreeRTOS scheduler.
 * ----------------------------------------------------------------------- */
void app_main(void)
{
    /* Visible LED pin — LEDC manages IRLED so do NOT call gpio_set_direction on it */
    gpio_set_direction((gpio_num_t)VISLED, GPIO_MODE_OUTPUT);

    /* Initialise LEDC timer and channel once — GPIO is claimed here only */
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)DUTY_RESOLUTION,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 38000,   /* default; overridden per-code by ledc_set_freq() */
        .clk_cfg         = LEDC_AUTO_CLK,
        .deconfigure     = false,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .gpio_num   = IRLED,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = (ledc_channel_t)PWM_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,   /* IR LED off at startup */
        .hpoint     = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags      = { .output_invert = 0 },
    };
    ledc_channel_config(&ledc_channel);
    ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)PWM_CHANNEL, 0);

    /* NA and EU pushbutton pins — inputs with internal pull-ups */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_NA) | (1ULL << BUTTON_EU),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    ESP_LOGI(TAG, ".....TV-B-Gone on an ESP32-C3 Super Mini board.....");
    ESP_LOGI(TAG, "                       V0.2");

    xTaskCreate(tvb_gone_task, "tvb_gone", 4096, NULL, 5, NULL);
}
