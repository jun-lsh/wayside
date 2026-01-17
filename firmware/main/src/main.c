#include "nvs_flash.h"
#include "esp_log.h"
#include "wifi_task.h"
#include "espnow.h"
#include "keygen.h"
#include "ble_task.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
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

    wifi_init();
    espnow_init();
    ble_init();
}
