#include "nfc_pair.h"
#include "name.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "nfc_pair";

static const uint8_t NDEF_TYPE_BLE_OOB[] = "application/vnd.bluetooth.le.oob";
#define NDEF_TYPE_BLE_OOB_LEN  32

#define BLE_OOB_LE_ROLE         0x1C
#define BLE_OOB_LOCAL_NAME      0x09
#define BLE_OOB_BD_ADDR         0x1B
#define LE_ROLE_PERIPHERAL_ONLY 0x00
#define NDEF_BLOCK_START        1

static nfc_pair_config_t s_config;
static nfc_pair_state_t s_state = NFC_PAIR_IDLE;
static char s_device_name[NAME_MAX_LEN] = {0};
static TimerHandle_t s_timeout_timer = NULL;
static bool s_initialized = false;

static size_t build_ble_ndef(uint8_t *buf, size_t buf_len)
{
    if (!buf || buf_len < 128) return 0; 
    
    size_t pos = 0;
    size_t name_len = strlen(s_device_name);
    if (name_len > 20) name_len = 20;
    
    size_t payload_len = 3 + 9 + 2 + name_len;
    
    buf[pos++] = 0x03;
    
    size_t record_len = 1 + 1 + 1 + NDEF_TYPE_BLE_OOB_LEN + payload_len;
    if (record_len > 254) {
        ESP_LOGE(TAG, "ndef too long");
        return 0;
    }
    buf[pos++] = (uint8_t)record_len;
    
    buf[pos++] = 0xD2;
    buf[pos++] = NDEF_TYPE_BLE_OOB_LEN;
    buf[pos++] = (uint8_t)payload_len;
    
    memcpy(&buf[pos], NDEF_TYPE_BLE_OOB, NDEF_TYPE_BLE_OOB_LEN);
    pos += NDEF_TYPE_BLE_OOB_LEN;
    
    buf[pos++] = 2;
    buf[pos++] = BLE_OOB_LE_ROLE;
    buf[pos++] = LE_ROLE_PERIPHERAL_ONLY;
    
    buf[pos++] = 8;
    buf[pos++] = BLE_OOB_BD_ADDR;
    for (int i = 5; i >= 0; i--) {
        buf[pos++] = s_config.ble_mac[i];
    }
    buf[pos++] = 0x00;
    
    buf[pos++] = 1 + name_len;
    buf[pos++] = BLE_OOB_LOCAL_NAME;
    memcpy(&buf[pos], s_device_name, name_len);
    pos += name_len;
    
    buf[pos++] = 0xFE;
    
    return pos;
}

static size_t build_default_ndef(uint8_t *buf, size_t buf_len)
{
    if (!buf || buf_len < 32) return 0;
    
    const char *uri = "wayside.com";
    size_t uri_len = strlen(uri);
    size_t pos = 0;
    
    buf[pos++] = 0x03;
    buf[pos++] = 1 + 1 + 1 + 1 + 1 + uri_len;
    buf[pos++] = 0xD1;
    buf[pos++] = 0x01;
    buf[pos++] = 1 + uri_len;
    buf[pos++] = 'U';
    buf[pos++] = 0x01;
    memcpy(&buf[pos], uri, uri_len);
    pos += uri_len;
    buf[pos++] = 0xFE;
    
    return pos;
}

static void timeout_callback(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "timeout");
    nfc_pair_clear_ndef();
}

static void set_state(nfc_pair_state_t new_state)
{
    if (s_state != new_state) {
        s_state = new_state;
        if (s_config.callback) {
            s_config.callback(new_state, s_config.cb_arg);
        }
    }
}

esp_err_t nfc_pair_init(const nfc_pair_config_t *config)
{
    if (!config || !config->nfc) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    memcpy(&s_config, config, sizeof(nfc_pair_config_t));
    
    // try name.h first, fallback to config
    esp_err_t ret = name_get(0, s_device_name, sizeof(s_device_name));
    if (ret != ESP_OK && config->device_name) {
        strncpy(s_device_name, config->device_name, sizeof(s_device_name) - 1);
        s_device_name[sizeof(s_device_name) - 1] = '\0';
    }
    
    if (config->otp_refresh_ms > 0) {
        s_timeout_timer = xTimerCreate("nfc_to", 
                                        pdMS_TO_TICKS(config->otp_refresh_ms),
                                        pdTRUE, NULL, timeout_callback);
    }
    
    s_state = NFC_PAIR_IDLE;
    s_initialized = true;
    
    ESP_LOGI(TAG, "init ok (name=%s, mac=%02x:%02x:%02x:%02x:%02x:%02x)",
             s_device_name,
             config->ble_mac[0], config->ble_mac[1], config->ble_mac[2],
             config->ble_mac[3], config->ble_mac[4], config->ble_mac[5]);
    
    return ESP_OK;
}

esp_err_t nfc_pair_write_ndef(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t block0[16];
    nfc_read_block(s_config.nfc, 0, block0, false);
    block0[0] = 0xAA;

    if (block0[12] != 0xE1 || block0[13] != 0x10 || block0[14] != 0x6D) {
        ESP_LOGI(TAG, "configuring cc");
        block0[12] = 0xE1; 
        block0[13] = 0x10; 
        block0[14] = 0x6D; 
        block0[15] = 0x00; 
        nfc_write_block(s_config.nfc, 0, block0, false);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    uint8_t ndef_buf[128];
    size_t ndef_len = build_ble_ndef(ndef_buf, sizeof(ndef_buf));
    if (ndef_len == 0) {
        ESP_LOGE(TAG, "build ndef failed");
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    
    ESP_LOGI(TAG, "writing ndef (%d bytes)", ndef_len);
    
    nfc_write_bytes(s_config.nfc, NDEF_BLOCK_START, ndef_buf, ndef_len);

    vTaskDelay(pdMS_TO_TICKS(10));
    
    nfc_set_fd_mode(s_config.nfc, NFC_FD_OFF_LAST_NDEF, NFC_FD_ON_RF_ON);
    
    uint8_t last_block = NDEF_BLOCK_START + (ndef_len / NFC_BLOCK_SIZE);
    nfc_set_last_ndef_block(s_config.nfc, last_block);
    
    set_state(NFC_PAIR_READY);
    
    if (s_timeout_timer && s_config.otp_refresh_ms > 0) {
        xTimerStart(s_timeout_timer, 0);
    }
    
    ESP_LOGI(TAG, "ndef written, ready for tap");
    return ESP_OK;
}

uint32_t nfc_pair_get_otp(void)
{
    return 0;
}

void nfc_pair_get_otp_str(char *buf, size_t buf_len)
{
    if (buf && buf_len > 0) {
        buf[0] = '\0';
    }
}

nfc_pair_state_t nfc_pair_get_state(void)
{
    return s_state;
}

bool nfc_pair_rf_present(void)
{
    if (!s_initialized) return false;
    return nfc_rf_present(s_config.nfc);
}

esp_err_t nfc_pair_start_advertising(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    set_state(NFC_PAIR_ADVERTISING);
    return ESP_OK;
}

esp_err_t nfc_pair_stop_advertising(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    set_state(NFC_PAIR_READY);
    return ESP_OK;
}

esp_err_t nfc_pair_clear_ndef(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_timeout_timer) {
        xTimerStop(s_timeout_timer, 0);
    }
    
    uint8_t ndef_buf[128];
    size_t ndef_len = build_default_ndef(ndef_buf, sizeof(ndef_buf));
    
    esp_err_t ret = nfc_write_bytes(s_config.nfc, NDEF_BLOCK_START, ndef_buf, ndef_len);
    if (ret != ESP_OK) {
        return ret;
    }
    
    set_state(NFC_PAIR_IDLE);
    return ESP_OK;
}

void nfc_pair_deinit(void)
{
    if (!s_initialized) return;
    
    if (s_timeout_timer) {
        xTimerDelete(s_timeout_timer, 0);
        s_timeout_timer = NULL;
    }
    
    s_initialized = false;
    s_state = NFC_PAIR_IDLE;
}
