#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TVBGONE_CORE_DEFAULT_VISLED_GPIO GPIO_NUM_8
#define TVBGONE_CORE_DEFAULT_IRLED_GPIO GPIO_NUM_2
#define TVBGONE_CORE_DEFAULT_BUTTON_NA_GPIO GPIO_NUM_10
#define TVBGONE_CORE_DEFAULT_BUTTON_EU_GPIO GPIO_NUM_9
#define TVBGONE_CORE_DEFAULT_TASK_STACK_SIZE 4096U
#define TVBGONE_CORE_DEFAULT_TASK_PRIORITY 5U

typedef struct {
    gpio_num_t visible_led_gpio;
    gpio_num_t ir_led_gpio;
    gpio_num_t button_na_gpio;
    gpio_num_t button_eu_gpio;
    bool visible_led_active_low;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
} tvbgone_core_config_t;

void tvbgone_core_get_default_config(tvbgone_core_config_t *config);
esp_err_t tvbgone_core_start(const tvbgone_core_config_t *config);

#ifdef __cplusplus
}
#endif
