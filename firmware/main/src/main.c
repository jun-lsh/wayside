#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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
#include "monitor.h"
#include "nfc.h"
#include "nfc_pair.h"

static const char *TAG = "main";

// i2c bus handle for nfc
extern i2c_master_bus_handle_t hnr26_badge_bus_handle;

// nfc handle
static nfc_t s_nfc;

// monitor queue handle
static QueueHandle_t s_monitor_queue = NULL;

// nfc pairing state callback
static void nfc_pair_callback(nfc_pair_state_t state, void *arg)
{
    switch (state) {
        case NFC_PAIR_IDLE:
            ESP_LOGI(TAG, "nfc: idle");
            break;
        case NFC_PAIR_ADVERTISING:
            ESP_LOGI(TAG, "nfc: advertising");
            break;
        case NFC_PAIR_NDEF_WRITTEN:
            ESP_LOGI(TAG, "nfc: ndef written, tap phone to pair");
            break;
        case NFC_PAIR_PAIRED:
            ESP_LOGI(TAG, "nfc: paired");
            break;
    }
}

// init nfc if tag is connected
static esp_err_t nfc_init_if_connected(void)
{
    esp_err_t ret;

    // init nfc driver
    ret = nfc_init(&s_nfc, hnr26_badge_bus_handle, NFC_I2C_ADDR, NFC_I2C_FREQ_HZ, NFC_FD_PIN);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nfc init failed (tag not connected?): %s", esp_err_to_name(ret));
        return ret;
    }
    
    // try to read a block to verify connection
    uint8_t test_buf[16];
    ret = nfc_read_block(&s_nfc, 0, test_buf, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nfc read failed (tag not responding): %s", esp_err_to_name(ret));
        nfc_deinit(&s_nfc);
        return ret;
    }
    
    ESP_LOGI(TAG, "nfc tag detected and initialized");
    return ESP_OK;
}

// setup nfc ble pairing (call after ble_init)
static esp_err_t setup_nfc_pairing(void)
{
    uint8_t ble_mac[6];
    esp_err_t ret = ble_get_mac(ble_mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to get ble mac");
        return ret;
    }
    
    const char *name = ble_get_device_name();
    
    nfc_pair_config_t pair_cfg = {
        .nfc = &s_nfc,
        .device_name = name,
        .ndef_timeout_ms = 10000,  // clear ndef after 10 seconds
        .callback = nfc_pair_callback,
        .cb_arg = NULL,
    };
    memcpy(pair_cfg.ble_mac, ble_mac, 6);
    
    ret = nfc_pair_init(&pair_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nfc pair init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // write pairing ndef immediately (ble is already advertising)
    ret = nfc_pair_write_ndef();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "failed to write pairing ndef: %s", esp_err_to_name(ret));
    }
    
    return ESP_OK;
}

void app_main(void)
{
    // configure nfc power enable pin
    gpio_config_t gpio0_conf = {
        .pin_bit_mask = (1ULL << NFC_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t ret = gpio_config(&gpio0_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure nfc power pin: %s", esp_err_to_name(ret));
        return;
    }
    ret = gpio_set_level(NFC_PWR_PIN, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable nfc power: %s", esp_err_to_name(ret));
        return;
    }

    // init nvs
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // init buzzer
    buzzer_config_t config = {
        .gpio_num = 3,
        .frequency = 2700,
        .initial_volume = 100
    };
    ret = buzzer_init(&config);

    // init badge leds
    ret = hnr26_badge_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "badge init failed: %s (leds may not work)", esp_err_to_name(ret));
    }

    // init proximity sensor
    ret = proximity_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "proximity init failed: %s", esp_err_to_name(ret));
    }

    // init monitor task for voltage and temperature logging
    ret = monitor_init(VBAT_ADC_CHANNEL, &s_monitor_queue);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "monitor init failed: %s", esp_err_to_name(ret));
    }

    // init wireless
    wifi_init();
    espnow_init();
    ble_init();
    
    // init nfc and setup pairing
    ret = nfc_init_if_connected();
    if (ret == ESP_OK) {
        setup_nfc_pairing();
    }
}

