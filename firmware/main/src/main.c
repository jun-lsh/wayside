/*
 * main.c - NFC-triggered BLE pairing example
 * 
 * Flow:
 * 1. Initialize NFC and BLE
 * 2. Write pairing NDEF to NFC tag (device name + OTP)
 * 3. Wait for phone tap (FD pin)
 * 4. Start BLE advertising with OTP as passkey
 * 5. Phone connects using OTP from NFC
 * 6. After BLE connection, exchange RSA keys
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "wifi_task.h"
#include "espnow.h"
#include "ble_task.h"
#include "definitions.h"
#include "buzzer.h"
#include "hnr26_badge.h"
#include "proximity.h"
#include "monitor.h"
#include "nfc.h"
#include "nfc_pair.h"

static const char *TAG = "main";

// I2C bus handle for NFC
extern i2c_master_bus_handle_t hnr26_badge_bus_handle;

// NFC handle
static nfc_t s_nfc;

// Monitor queue
static QueueHandle_t s_monitor_queue = NULL;

/**
 * NFC pairing state callback
 */
static void nfc_pair_callback(nfc_pair_state_t state, void *arg)
{
    switch (state) {
        case NFC_PAIR_IDLE:
            ESP_LOGI(TAG, "NFC Pair: Idle");
            break;
            
        case NFC_PAIR_READY:
            ESP_LOGI(TAG, "NFC Pair: Ready - NDEF written, waiting for phone tap");
            // Could show LED pattern indicating ready state
            break;
            
        case NFC_PAIR_PHONE_DETECTED:
            ESP_LOGI(TAG, "NFC Pair: Phone detected!");
            // Could buzz or flash LED
            buzzer_beep(100, 2700, 100);
            break;
            
        case NFC_PAIR_ADVERTISING:
            ESP_LOGI(TAG, "NFC Pair: BLE advertising started");
            break;
            
        case NFC_PAIR_CONNECTED:
            ESP_LOGI(TAG, "NFC Pair: BLE connected");
            break;
            
        case NFC_PAIR_AUTHENTICATED:
            ESP_LOGI(TAG, "NFC Pair: Authenticated!");
            // Pairing complete - phone can now send commands
            buzzer_beep(50, 3000, 100);
            vTaskDelay(pdMS_TO_TICKS(150));
            buzzer_beep(50, 3500, 100);
            break;
    }
}

/**
 * BLE connection callback
 */
static void ble_connection_callback(bool connected, void *arg)
{
    if (connected) {
        ESP_LOGI(TAG, "BLE: Device connected");
    } else {
        ESP_LOGI(TAG, "BLE: Device disconnected");
    }
}

/**
 * BLE authentication callback
 */
static void ble_auth_callback(bool success, void *arg)
{
    if (success) {
        ESP_LOGI(TAG, "BLE: Authentication successful");
    } else {
        ESP_LOGW(TAG, "BLE: Authentication failed");
    }
}

/**
 * Initialize NFC if tag is connected
 */
static esp_err_t init_nfc(void)
{
    esp_err_t ret = nfc_init(&s_nfc, hnr26_badge_bus_handle, 
                              NFC_I2C_ADDR, NFC_I2C_FREQ_HZ, NFC_FD_PIN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NFC init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "NFC tag initialized");
    return ESP_OK;
}

/**
 * Setup NFC pairing (call after ble_init)
 */
static esp_err_t setup_nfc_pairing(void)
{
    uint8_t ble_mac[6];
    esp_err_t ret = ble_get_mac(ble_mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get BLE MAC");
        return ret;
    }
    
    const char *name = ble_get_device_name();
    
    nfc_pair_config_t cfg = {
        .nfc = &s_nfc,
        .device_name = name,
        .adv_timeout_sec = 60,          // Advertise for 60 seconds after tap
        .otp_refresh_ms = 5 * 60 * 1000, // Refresh OTP every 5 minutes
        .callback = nfc_pair_callback,
        .cb_arg = NULL,
    };
    memcpy(cfg.ble_mac, ble_mac, 6);
    
    ret = nfc_pair_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NFC pair init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Write initial pairing NDEF
    ret = nfc_pair_write_ndef();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to write pairing NDEF: %s", esp_err_to_name(ret));
    } else {
        char otp_str[8];
        nfc_pair_get_otp_str(otp_str, sizeof(otp_str));
        ESP_LOGI(TAG, "NFC pairing ready. OTP: %s", otp_str);
    }
    
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t ret;
    
    // === Power on NFC ===
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << NFC_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(NFC_PWR_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "NFC power on");
    
    // === Initialize NVS ===
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // === Initialize peripherals ===
    buzzer_config_t buzz_cfg = {
        .gpio_num = 3,
        .frequency = 2700,
        .initial_volume = 100
    };
    buzzer_init(&buzz_cfg);
    
    hnr26_badge_init();
    proximity_init(NULL);
    monitor_init(VBAT_ADC_CHANNEL, &s_monitor_queue);
    
    // === Initialize wireless ===
    wifi_init();
    espnow_init();
    
    // Initialize BLE (does not start advertising yet)
    ret = ble_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    // Set BLE callbacks
    ble_set_connection_callback(ble_connection_callback, NULL);
    ble_set_auth_callback(ble_auth_callback, NULL);
    
    // === Initialize NFC pairing ===
    ret = init_nfc();
    if (ret == ESP_OK) {
        setup_nfc_pairing();
    } else {
        // No NFC - fall back to manual BLE pairing (Just Works)
        ESP_LOGW(TAG, "NFC not available, starting BLE without passkey");
        ble_start_pairing(0);  // No timeout, Just Works mode
    }
    
    // Startup beep
    buzzer_beep(100, 2700, 100);
    
    ESP_LOGI(TAG, "=== Ready ===");
    ESP_LOGI(TAG, "Tap phone on NFC tag to pair via BLE");
}
