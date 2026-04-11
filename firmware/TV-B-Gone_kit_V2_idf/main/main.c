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
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rmt_common.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "TV-B-Gone";

/* GPIO level aliases */
#define LOW  0
#define HIGH 1

/* Timing constants (milliseconds) */
#define VIS_LED_BLIP_TIME    25   /* LED blip before each POWER-Code */
#define TIME_BETWEEN_CODES  205   /* gap between POWER-Codes */
#define BLIP_TIME           125   /* delay between indication blinks */

/* RMT timing constants */
#define RMT_RESOLUTION_HZ        1000000U
#define RMT_MEM_BLOCK_SYMBOLS    128
#define RMT_MAX_DURATION_TICKS   32767U
#define RMT_MAX_SYMBOLS          1280U

/* Power-code arrays defined in WORLDcodes.cpp (extern "C" there → plain C here) */
extern struct IrCode *NApowerCodes[];
extern struct IrCode *EUpowerCodes[];
extern uint8_t num_NAcodes;
extern uint8_t num_EUcodes;

static rmt_channel_handle_t ir_rmt_channel;
static rmt_encoder_handle_t ir_copy_encoder;
static rmt_symbol_word_t ir_symbols[RMT_MAX_SYMBOLS];

/* Forward declarations */
static size_t append_half_level(rmt_symbol_word_t *symbols, size_t symbol_index,
                                bool *has_pending_half, bool *pending_level,
                                uint16_t *pending_duration, uint32_t duration_us,
                                bool level);
static size_t build_power_code_symbols(rmt_symbol_word_t *symbols, size_t max_symbols,
                                       int numPairs, uint32_t *pairsTab_ptr,
                                       uint8_t *sequenceTab_ptr);
static void xmitCode(uint32_t carrierFreq, int numPairs, uint32_t *pairsTab_ptr,
                     uint8_t *sequenceTab_ptr);
static void blinkLEDnTimes(int numBlinks);
static void tvb_gone_task(void *pvParameters);


/* -----------------------------------------------------------------------
 * append_half_level()
 *
 * Append one logical level segment to the RMT stream, splitting long
 * durations across multiple half-symbol slots while preserving the level.
 * ----------------------------------------------------------------------- */
static size_t append_half_level(rmt_symbol_word_t *symbols, size_t symbol_index,
                                bool *has_pending_half, bool *pending_level,
                                uint16_t *pending_duration, uint32_t duration_us,
                                bool level)
{
    while (duration_us > 0) {
        uint16_t chunk_ticks = (duration_us > RMT_MAX_DURATION_TICKS)
                                   ? (uint16_t)RMT_MAX_DURATION_TICKS
                                   : (uint16_t)duration_us;

        if (!*has_pending_half) {
            *pending_level = level;
            *pending_duration = chunk_ticks;
            *has_pending_half = true;
        } else {
            symbols[symbol_index].level0 = *pending_level;
            symbols[symbol_index].duration0 = *pending_duration;
            symbols[symbol_index].level1 = level;
            symbols[symbol_index].duration1 = chunk_ticks;
            symbol_index++;
            *has_pending_half = false;
        }

        duration_us -= chunk_ticks;
    }

    return symbol_index;
}

/* -----------------------------------------------------------------------
 * build_power_code_symbols()
 *
 * Convert a POWER-Code's mark/space sequence into an RMT symbol array.
 * ----------------------------------------------------------------------- */
static size_t build_power_code_symbols(rmt_symbol_word_t *symbols, size_t max_symbols,
                                       int numPairs, uint32_t *pairsTab_ptr,
                                       uint8_t *sequenceTab_ptr)
{
    size_t symbol_index = 0;
    bool has_pending_half = false;
    bool pending_level = false;
    uint16_t pending_duration = 0;

    for (int i = 0; i < numPairs; i++) {
        uint8_t pairsIndex = sequenceTab_ptr[i] * 2;
        uint32_t onTime = pairsTab_ptr[pairsIndex];
        uint32_t offTime = pairsTab_ptr[pairsIndex + 1];

        DEBUGP(ESP_LOGI(TAG, "pair[%d] = %" PRIu32 ", %" PRIu32, i, onTime, offTime));

        symbol_index = append_half_level(symbols, symbol_index, &has_pending_half,
                                         &pending_level, &pending_duration,
                                         onTime, true);
        symbol_index = append_half_level(symbols, symbol_index, &has_pending_half,
                                         &pending_level, &pending_duration,
                                         offTime, false);

        if (symbol_index >= max_symbols) {
            ESP_LOGE(TAG, "RMT symbol buffer overflow while encoding POWER-Code");
            return 0;
        }
    }

    if (has_pending_half) {
        if (symbol_index >= max_symbols) {
            ESP_LOGE(TAG, "RMT symbol buffer overflow while finalizing POWER-Code");
            return 0;
        }
        symbols[symbol_index].level0 = pending_level;
        symbols[symbol_index].duration0 = pending_duration;
        symbols[symbol_index].level1 = LOW;
        symbols[symbol_index].duration1 = 0;
        symbol_index++;
    }

    return symbol_index;
}

/* -----------------------------------------------------------------------
 * xmitCode()
 *
 * Transmit one full POWER-Code using the ESP-IDF 5.5 RMT TX driver.
 * ----------------------------------------------------------------------- */
static void xmitCode(uint32_t carrierFreq, int numPairs, uint32_t *pairsTab_ptr,
                     uint8_t *sequenceTab_ptr)
{
    size_t symbol_count = build_power_code_symbols(ir_symbols, RMT_MAX_SYMBOLS,
                                                   numPairs, pairsTab_ptr,
                                                   sequenceTab_ptr);
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
        .flags = {
            .eot_level = LOW,
            .queue_nonblocking = 0,
        },
    };

    if (symbol_count == 0) {
        ESP_LOGE(TAG, "Skipping POWER-Code transmit because symbol build failed");
        return;
    }

    if (carrierFreq != 0) {
        rmt_carrier_config_t carrier_cfg = {
            .frequency_hz = carrierFreq,
            .duty_cycle = 0.5f,
            .flags = {
                .polarity_active_low = 0,
                .always_on = 0,
            },
        };
        ESP_ERROR_CHECK(rmt_apply_carrier(ir_rmt_channel, &carrier_cfg));
    } else {
        ESP_ERROR_CHECK(rmt_apply_carrier(ir_rmt_channel, NULL));
    }

    DEBUGP(ESP_LOGI(TAG, "Built %u RMT symbols for %" PRIu32 " Hz carrier",
                    (unsigned int)symbol_count, carrierFreq));

    ESP_ERROR_CHECK(rmt_transmit(ir_rmt_channel, ir_copy_encoder, ir_symbols,
                                 symbol_count * sizeof(ir_symbols[0]), &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(ir_rmt_channel, -1));
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
    (void)pvParameters;
    int buttonStateNA;
    int buttonStateEU;
    int buttonStateNA_again;
    int buttonStateEU_again;
    int region = NA;
    int startOver = FALSE;
    int numCodes = 0;
    int numPairs;
    uint32_t carrierFreq;
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
                    xmitCode(carrierFreq, numPairs, pairsTab_ptr, sequenceTab_ptr);

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
    /* Visible LED pin */
    gpio_set_direction((gpio_num_t)VISLED, GPIO_MODE_OUTPUT);

    /* Initialise one reusable RMT TX channel for the IR LED */
    rmt_tx_channel_config_t tx_chan_cfg = {
        .gpio_num = (gpio_num_t)IRLED,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS,
        .trans_queue_depth = 1,
        .intr_priority = 0,
        .flags = {
            .invert_out = 0,
            .with_dma = 0,
            .io_loop_back = 0,
            .io_od_mode = 0,
            .allow_pd = 0,
        },
    };
    rmt_copy_encoder_config_t copy_encoder_cfg = {};
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_cfg, &ir_rmt_channel));
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_cfg, &ir_copy_encoder));
    ESP_ERROR_CHECK(rmt_enable(ir_rmt_channel));

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
