#include "tvbgone_core.h"

#include "esp_err.h"

void app_main(void)
{
    tvbgone_core_config_t config;

    tvbgone_core_get_default_config(&config);
    config.visible_led_gpio = TVBGONE_CORE_DEFAULT_VISLED_GPIO;
    config.ir_led_gpio = TVBGONE_CORE_DEFAULT_IRLED_GPIO;
    config.button_na_gpio = TVBGONE_CORE_DEFAULT_BUTTON_NA_GPIO;
    config.button_eu_gpio = TVBGONE_CORE_DEFAULT_BUTTON_EU_GPIO;
    config.visible_led_active_low = true;

    ESP_ERROR_CHECK(tvbgone_core_start(&config));
}
