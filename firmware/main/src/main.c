#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "wifi_task.h"
#include "espnow.h"
#include "keygen.h"
#include "ble_task.h"
#include "definitions.h"
#include "buzzer.h"
#include "hnr26_badge.h"
#include "proximity.h"

static const char *TAG = "main";


void app_main(void)
{
    // temp
    gpio_config_t gpio0_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t ret = gpio_config(&gpio0_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO 0: %s", esp_err_to_name(ret));
        return;
    }
    ret = gpio_set_level(GPIO_NUM_0, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set GPIO 0 high: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // rsa_key_pair_t *global_keys = malloc(sizeof(rsa_key_pair_t));
    // if (global_keys == NULL) {
    //     ESP_LOGE(TAG, "Failed to allocate memory for keys");
    //     return;
    // }

    // if (load_or_generate_keypair(global_keys) != 0) {
    //     ESP_LOGE(TAG, "Key loading/generation failed, cannot start tasks.");
    //     free(global_keys);
    //     return;
    // }

    
    buzzer_config_t config = {
        .gpio_num = 3,
        .frequency = 2700,
        .initial_volume = 100
    };
    ret = buzzer_init(&config);

    ret = hnr26_badge_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Badge init failed: %s (LEDs may not work)", esp_err_to_name(ret));
    }

    ret = proximity_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Proximity init failed: %s", esp_err_to_name(ret));
    }

    // nfc_init_if_connected();
    wifi_init();
    espnow_init();
    ble_init();
}
