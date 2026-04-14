/*
  TV-B-Gone reusable component for ESP-IDF.

  This component preserves the current TV-B-Gone ESP-IDF behavior while
  exposing a small configuration-driven API for example apps and reuse.
*/

#include "tvbgone_core.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_common.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tvbgone_core_internal.h"

static const char *TAG = "tvbgone_core";

#define TVBGONE_REGION_NA 1
#define TVBGONE_REGION_EU 0

#define LOW 0
#define HIGH 1

#define VIS_LED_BLIP_TIME_MS 25
#define TIME_BETWEEN_CODES_MS 205
#define BLIP_TIME_MS 125

#define RMT_RESOLUTION_HZ 1000000U
#define RMT_MEM_BLOCK_SYMBOLS 128
#define RMT_MAX_DURATION_TICKS 32767U
#define RMT_MAX_SYMBOLS 1280U

typedef struct {
    tvbgone_core_config_t config;
    rmt_channel_handle_t ir_rmt_channel;
    rmt_encoder_handle_t ir_copy_encoder;
    rmt_symbol_word_t ir_symbols[RMT_MAX_SYMBOLS];
    TaskHandle_t task_handle;
    bool started;
} tvbgone_core_ctx_t;

static tvbgone_core_ctx_t s_ctx = {
    .config = {
        .visible_led_gpio = TVBGONE_CORE_DEFAULT_VISLED_GPIO,
        .ir_led_gpio = TVBGONE_CORE_DEFAULT_IRLED_GPIO,
        .button_na_gpio = TVBGONE_CORE_DEFAULT_BUTTON_NA_GPIO,
        .button_eu_gpio = TVBGONE_CORE_DEFAULT_BUTTON_EU_GPIO,
        .visible_led_active_low = true,
        .task_stack_size = TVBGONE_CORE_DEFAULT_TASK_STACK_SIZE,
        .task_priority = TVBGONE_CORE_DEFAULT_TASK_PRIORITY,
    },
};

static int visible_led_on_level(void)
{
    return s_ctx.config.visible_led_active_low ? LOW : HIGH;
}

static int visible_led_off_level(void)
{
    return s_ctx.config.visible_led_active_low ? HIGH : LOW;
}

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

static size_t build_power_code_symbols(rmt_symbol_word_t *symbols, size_t max_symbols,
                                       int num_pairs, uint32_t *pairs_tab_ptr,
                                       uint8_t *sequence_tab_ptr)
{
    size_t symbol_index = 0;
    bool has_pending_half = false;
    bool pending_level = false;
    uint16_t pending_duration = 0;

    for (int i = 0; i < num_pairs; i++) {
        uint8_t pairs_index = sequence_tab_ptr[i] * 2;
        uint32_t on_time = pairs_tab_ptr[pairs_index];
        uint32_t off_time = pairs_tab_ptr[pairs_index + 1];

        symbol_index = append_half_level(symbols, symbol_index, &has_pending_half,
                                         &pending_level, &pending_duration,
                                         on_time, true);
        symbol_index = append_half_level(symbols, symbol_index, &has_pending_half,
                                         &pending_level, &pending_duration,
                                         off_time, false);

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

static void xmit_code(uint32_t carrier_freq, int num_pairs, uint32_t *pairs_tab_ptr,
                      uint8_t *sequence_tab_ptr)
{
    size_t symbol_count = build_power_code_symbols(s_ctx.ir_symbols, RMT_MAX_SYMBOLS,
                                                   num_pairs, pairs_tab_ptr,
                                                   sequence_tab_ptr);
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

    if (carrier_freq != 0) {
        rmt_carrier_config_t carrier_cfg = {
            .frequency_hz = carrier_freq,
            .duty_cycle = 0.5f,
            .flags = {
                .polarity_active_low = 0,
                .always_on = 0,
            },
        };
        ESP_ERROR_CHECK(rmt_apply_carrier(s_ctx.ir_rmt_channel, &carrier_cfg));
    } else {
        ESP_ERROR_CHECK(rmt_apply_carrier(s_ctx.ir_rmt_channel, NULL));
    }

    ESP_LOGD(TAG, "Built %u RMT symbols for %" PRIu32 " Hz carrier",
             (unsigned int)symbol_count, carrier_freq);

    ESP_ERROR_CHECK(rmt_transmit(s_ctx.ir_rmt_channel, s_ctx.ir_copy_encoder,
                                 s_ctx.ir_symbols,
                                 symbol_count * sizeof(s_ctx.ir_symbols[0]),
                                 &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s_ctx.ir_rmt_channel, -1));
}

static void blink_led_n_times(int num_blinks)
{
    for (int i = 0; i < num_blinks; i++) {
        gpio_set_level(s_ctx.config.visible_led_gpio, visible_led_on_level());
        vTaskDelay(pdMS_TO_TICKS(VIS_LED_BLIP_TIME_MS));
        gpio_set_level(s_ctx.config.visible_led_gpio, visible_led_off_level());
        vTaskDelay(pdMS_TO_TICKS(BLIP_TIME_MS));
    }
}

static esp_err_t init_hardware(void)
{
    gpio_config_t vis_led_conf = {
        .pin_bit_mask = 1ULL << s_ctx.config.visible_led_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&vis_led_conf), TAG,
                        "failed to configure visible LED GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(s_ctx.config.visible_led_gpio,
                                       visible_led_off_level()),
                        TAG, "failed to set visible LED idle state");

    rmt_tx_channel_config_t tx_chan_cfg = {
        .gpio_num = s_ctx.config.ir_led_gpio,
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

    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_chan_cfg, &s_ctx.ir_rmt_channel), TAG,
                        "failed to create RMT TX channel");
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&copy_encoder_cfg, &s_ctx.ir_copy_encoder),
                        TAG, "failed to create RMT encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_ctx.ir_rmt_channel), TAG,
                        "failed to enable RMT channel");

    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << s_ctx.config.button_na_gpio) |
                        (1ULL << s_ctx.config.button_eu_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&btn_conf), TAG,
                        "failed to configure button GPIOs");

    ESP_LOGI(TAG, "TV-B-Gone hardware configuration done");
    return ESP_OK;
}

static void tvbgone_task(void *pv_parameters)
{
    (void)pv_parameters;
    int button_state_na;
    int button_state_eu;
    int button_state_na_again;
    int button_state_eu_again;
    int region = TVBGONE_REGION_NA;
    int start_over = false;
    int num_codes = 0;
    int num_pairs;
    uint32_t carrier_freq;
    struct IrCode *pwr_code_ptr;
    uint32_t *pairs_tab_ptr;
    uint8_t *sequence_tab_ptr;

    for (;;) {
        button_state_na = gpio_get_level(s_ctx.config.button_na_gpio);
        button_state_eu = gpio_get_level(s_ctx.config.button_eu_gpio);

        do {
            if (start_over) {
                button_state_na = button_state_na_again;
                button_state_eu = button_state_eu_again;
                start_over = false;
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "Transmission restarting from the beginning");
                ESP_LOGI(TAG, "");
            }

            if ((!button_state_na || !button_state_eu) == HIGH) {
                vTaskDelay(pdMS_TO_TICKS(250));

                if (button_state_na == LOW) {
                    region = TVBGONE_REGION_NA;
                    num_codes = num_NAcodes;
                    blink_led_n_times(3);
                    ESP_LOGI(TAG, "NA button pressed");
                    ESP_LOGI(TAG, "# of NA POWER-Codes: %d", num_codes);
                    ESP_LOGI(TAG, "");
                } else if (button_state_eu == LOW) {
                    region = TVBGONE_REGION_EU;
                    num_codes = num_EUcodes;
                    blink_led_n_times(6);
                    ESP_LOGI(TAG, "EU button pressed");
                    ESP_LOGI(TAG, "# of EU POWER-Codes: %d", num_codes);
                    ESP_LOGI(TAG, "");
                } else {
                    blink_led_n_times(25);
                    ESP_LOGW(TAG, "Transmission started without a selected region");
                }

                vTaskDelay(pdMS_TO_TICKS(500));

                for (int power_code_count = 0; power_code_count < num_codes; power_code_count++) {
                    pwr_code_ptr = (region == TVBGONE_REGION_NA)
                                       ? NApowerCodes[power_code_count]
                                       : EUpowerCodes[power_code_count];

                    gpio_set_level(s_ctx.config.visible_led_gpio, visible_led_on_level());
                    vTaskDelay(pdMS_TO_TICKS(VIS_LED_BLIP_TIME_MS));
                    gpio_set_level(s_ctx.config.visible_led_gpio, visible_led_off_level());

                    carrier_freq = pwr_code_ptr->carrier_freq;
                    num_pairs = pwr_code_ptr->num_pairs;
                    pairs_tab_ptr = pwr_code_ptr->pairs;
                    sequence_tab_ptr = pwr_code_ptr->sequence;

                    ESP_LOGI(TAG, "Transmitting POWER-Code %d...", power_code_count);
                    xmit_code(carrier_freq, num_pairs, pairs_tab_ptr, sequence_tab_ptr);
                    ESP_LOGI(TAG, "Done");

                    vTaskDelay(pdMS_TO_TICKS(TIME_BETWEEN_CODES_MS - VIS_LED_BLIP_TIME_MS));

                    button_state_na_again = gpio_get_level(s_ctx.config.button_na_gpio);
                    button_state_eu_again = gpio_get_level(s_ctx.config.button_eu_gpio);
                    if ((!button_state_na_again || !button_state_eu_again) == HIGH) {
                        start_over = true;
                        break;
                    }
                }

                if (!start_over) {
                    ESP_LOGI(TAG, "");
                    if (button_state_na == LOW) {
                        ESP_LOGI(TAG, "Finished transmitting entire database for NA.");
                        blink_led_n_times(3);
                    } else if (button_state_eu == LOW) {
                        ESP_LOGI(TAG, "Finished transmitting entire database for EU.");
                        blink_led_n_times(6);
                    } else {
                        ESP_LOGI(TAG, "Finished transmitting with no active region.");
                        blink_led_n_times(25);
                    }
                    ESP_LOGI(TAG, "");
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            } else {
                gpio_set_level(s_ctx.config.visible_led_gpio, visible_led_off_level());
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } while (start_over);
    }
}

void tvbgone_core_get_default_config(tvbgone_core_config_t *config)
{
    if (config == NULL) {
        return;
    }

    *config = s_ctx.config;
}

esp_err_t tvbgone_core_start(const tvbgone_core_config_t *config)
{
    tvbgone_core_config_t effective_config;

    ESP_RETURN_ON_FALSE(!s_ctx.started, ESP_ERR_INVALID_STATE, TAG,
                        "component already started");

    if (config == NULL) {
        tvbgone_core_get_default_config(&effective_config);
    } else {
        effective_config = *config;
    }

    ESP_RETURN_ON_FALSE(effective_config.task_stack_size > 0, ESP_ERR_INVALID_ARG, TAG,
                        "task stack size must be greater than zero");

    s_ctx.config = effective_config;
    ESP_RETURN_ON_ERROR(init_hardware(), TAG, "failed to initialize hardware");

    BaseType_t task_created = xTaskCreate(tvbgone_task, "tvbgone", s_ctx.config.task_stack_size,
                                          NULL, s_ctx.config.task_priority, &s_ctx.task_handle);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_FAIL, TAG,
                        "failed to create TV-B-Gone task");

    s_ctx.started = true;
    return ESP_OK;
}
