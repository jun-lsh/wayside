/*
 * nfc_pair.c - ble pairing via nfc using ntag i2c plus
 * 
 * implements bluetooth oob pairing ndef record per nfc forum spec.
 * based on nxp application note for kw41z + ntag i2c plus.
 */

#include "nfc_pair.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "nfc_pair";

// ndef record type for bluetooth le oob
static const uint8_t NDEF_TYPE_BLE_OOB[] = "application/vnd.bluetooth.le.oob";
#define NDEF_TYPE_BLE_OOB_LEN  32

// bluetooth le oob data type values
#define BLE_OOB_LE_ROLE         0x1C
#define BLE_OOB_LOCAL_NAME      0x09  // complete local name
#define BLE_OOB_BD_ADDR         0x1B  // le bluetooth device address

// le role values
#define LE_ROLE_PERIPHERAL_ONLY 0x00

// ndef message structure offsets
#define NDEF_BLOCK_START        1     // start writing ndef at block 1 (after cc)

// module state
static nfc_pair_config_t s_config;
static nfc_pair_state_t s_state = NFC_PAIR_IDLE;
static TimerHandle_t s_timeout_timer = NULL;
static bool s_initialized = false;

// build ndef message for ble oob pairing
// returns length of message, or 0 on error
static size_t build_ble_ndef(uint8_t *buf, size_t buf_len)
{
    if (!buf || buf_len < 128) return 0; 
    
    size_t pos = 0;
    
    // calculate payload length first
    // payload = le role (3) + bd addr (9) + local name (2 + name_len)
    size_t name_len = strlen(s_config.device_name);
    if (name_len > 20) name_len = 20;  // limit name length
    
    size_t payload_len = 3 + 9 + 2 + name_len;
    
    // ndef message header (tlv format for type 2 tags)
    buf[pos++] = 0x03;  // ndef message tlv type
    
    // ndef message length (record header + type + payload)
    // record header = 1 byte flags + 1 byte type len + 1 byte payload len (short) = 3
    size_t record_len = 1 + 1 + 1 + NDEF_TYPE_BLE_OOB_LEN + payload_len;
    if (record_len > 254) {
        ESP_LOGE(TAG, "ndef too long");
        return 0;
    }
    buf[pos++] = (uint8_t)record_len;
    
    // ndef record header
    // mb=1, me=1, cf=0, sr=1, il=0, tnf=2 (mime type)
    buf[pos++] = 0xD2;  // 1101 0010
    
    // type length
    buf[pos++] = NDEF_TYPE_BLE_OOB_LEN;
    
    // payload length (short record)
    buf[pos++] = (uint8_t)payload_len;
    
    // type
    memcpy(&buf[pos], NDEF_TYPE_BLE_OOB, NDEF_TYPE_BLE_OOB_LEN);
    pos += NDEF_TYPE_BLE_OOB_LEN;
    
    // payload: le bluetooth oob data
    
    // le role (required)
    buf[pos++] = 2;  // length
    buf[pos++] = BLE_OOB_LE_ROLE;
    buf[pos++] = LE_ROLE_PERIPHERAL_ONLY;
    
    // le bluetooth device address (required)
    // format: 6 bytes address + 1 byte address type (0=public, 1=random)
    buf[pos++] = 8;  // length (1 type + 6 addr + 1 addr type)
    buf[pos++] = BLE_OOB_BD_ADDR;
    // address is little endian
    for (int i = 5; i >= 0; i--) {
        buf[pos++] = s_config.ble_mac[i];
    }
    buf[pos++] = 0x00;  // public address
    
    // complete local name (optional but recommended)
    buf[pos++] = 1 + name_len;  // length
    buf[pos++] = BLE_OOB_LOCAL_NAME;
    memcpy(&buf[pos], s_config.device_name, name_len);
    pos += name_len;
    
    // terminator tlv
    buf[pos++] = 0xFE;
    
    return pos;
}

// nfc_pair.c

static size_t build_url_ndef(uint8_t *buf, size_t buf_len)
{
    if (!buf || buf_len < 128) return 0;
    
    // 1. Construct the URL
    // We want: https://keenbee.com/p?m=B0A6045540FE
    // (Keep it short! NTAGs are small, but shorter URLs scan faster)
    char url[64];
    snprintf(url, sizeof(url), "keenbee.com/p?m=%02X%02X%02X%02X%02X%02X", 
             s_config.ble_mac[0], s_config.ble_mac[1], s_config.ble_mac[2],
             s_config.ble_mac[3], s_config.ble_mac[4], s_config.ble_mac[5]);
             
    size_t url_len = strlen(url);
    size_t pos = 0;

    // --- NDEF HEADER ---
    
    // TLV: Type (0x03 = NDEF Message)
    buf[pos++] = 0x03; 
    
    // TLV: Length (Header + Type + Payload)
    // Header (1 byte flags) + TypeLen (1) + PayloadLen (1) + Type(1) + URI_ID(1) + URL
    size_t record_len = 1 + 1 + 1 + 1 + 1 + url_len; 
    buf[pos++] = (uint8_t)record_len;

    // --- NDEF RECORD ---

    // Record Header: MB=1, ME=1, SR=1, TNF=1 (Well Known)
    buf[pos++] = 0xD1; 
    
    // Type Length: 1 byte ('U')
    buf[pos++] = 0x01; 
    
    // Payload Length: (1 byte URI Identifier + URL string)
    buf[pos++] = 1 + url_len;
    
    // Type: 'U' (URI)
    buf[pos++] = 'U';
    
    // URI Identifier Code: 0x04 = "https://"
    // This saves 8 bytes of space!
    buf[pos++] = 0x04; 
    
    // Payload: The URL string
    memcpy(&buf[pos], url, url_len);
    pos += url_len;
    
    // TLV: Terminator
    buf[pos++] = 0xFE;
    
    return pos;
}

// build default ndef (simple text record)
static size_t build_default_ndef(uint8_t *buf, size_t buf_len)
{
    if (!buf || buf_len < 32) return 0;
    
    // simple uri record pointing to espressif
    const char *uri = "espressif.com";
    size_t uri_len = strlen(uri);
    
    size_t pos = 0;
    
    // ndef tlv
    buf[pos++] = 0x03;
    buf[pos++] = 1 + 1 + 1 + 1 + 1 + uri_len;  // record length
    
    // ndef record: uri
    buf[pos++] = 0xD1;  // mb=1, me=1, sr=1, tnf=1 (well-known)
    buf[pos++] = 0x01;  // type length (U)
    buf[pos++] = 1 + uri_len;  // payload length
    buf[pos++] = 'U';   // type = uri
    buf[pos++] = 0x01;  // uri prefix: http://www.
    memcpy(&buf[pos], uri, uri_len);
    pos += uri_len;
    
    // terminator
    buf[pos++] = 0xFE;
    
    return pos;
}

// timeout callback
static void timeout_callback(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "ndef timeout, clearing pairing data");
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
    if (!config || !config->nfc || !config->device_name) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    memcpy(&s_config, config, sizeof(nfc_pair_config_t));
    
    // create timeout timer if needed
    if (config->ndef_timeout_ms > 0) {
        s_timeout_timer = xTimerCreate("nfc_timeout", 
                                        pdMS_TO_TICKS(config->ndef_timeout_ms),
                                        pdFALSE,  // one-shot
                                        NULL, 
                                        timeout_callback);
        if (!s_timeout_timer) {
            ESP_LOGW(TAG, "failed to create timeout timer");
        }
    }
    
    s_state = NFC_PAIR_IDLE;
    s_initialized = true;
    
    ESP_LOGI(TAG, "nfc pairing init ok (mac=%02x:%02x:%02x:%02x:%02x:%02x, name=%s)",
             config->ble_mac[0], config->ble_mac[1], config->ble_mac[2],
             config->ble_mac[3], config->ble_mac[4], config->ble_mac[5],
             config->device_name);
    
    return ESP_OK;
}

esp_err_t nfc_pair_write_ndef(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t block0[16];
    nfc_read_block(s_config.nfc, 0, block0, false);

    // Check if CC is already correct (Bytes 12-15)
    // CC: Magic(0xE1) Ver(0x10) Size(0x6D) Access(0x00)
    if (block0[12] != 0xE1 || block0[13] != 0x10 || block0[14] != 0x6D) {
        ESP_LOGI(TAG, "Configuring CC file (Bytes 12-15)...");
        
        // Modify only CC bytes
        // Define standard CC for NTAG I2C Plus (1k/2k)
        // 0xE1 = Magic Number
        // 0x10 = Version 1.0
        // 0x6D = Size (872 bytes for 1k tag, safe default)
        // 0x00 = Read/Write access
        block0[12] = 0xE1; 
        block0[13] = 0x10; 
        block0[14] = 0x6D; 
        block0[15] = 0x00; 

        ESP_LOGI(TAG, "Configuring CC file at Block 0");
    
        // Write Block 0 directly. 
        // We use nfc_write_block instead of nfc_write_bytes because 
        // the helper function blocks writing to address 0.
        nfc_write_block(s_config.nfc, 0, block0, false);

        vTaskDelay(pdMS_TO_TICKS(50));
        
        ESP_LOGI(TAG, "CC file configured.");
    }
    
    uint8_t ndef_buf[128];
    //size_t ndef_len = build_ble_ndef(ndef_buf, sizeof(ndef_buf));
    size_t ndef_len = build_url_ndef(ndef_buf, sizeof(ndef_buf));
    if (ndef_len == 0) {
        ESP_LOGE(TAG, "failed to build ndef");
        return ESP_FAIL;
    }

    // use a small delay between block 0 write and block 1 write for safety
    vTaskDelay(pdMS_TO_TICKS(10));
    
    ESP_LOGI(TAG, "writing ble pairing ndef (%d bytes)", ndef_len);
    
    // write ndef to tag starting at block 1
    nfc_write_bytes(s_config.nfc, NDEF_BLOCK_START, ndef_buf, ndef_len);

    vTaskDelay(pdMS_TO_TICKS(10));
    
    // configure fd pin to signal when ndef is read
    nfc_set_fd_mode(s_config.nfc, NFC_FD_OFF_LAST_NDEF, NFC_FD_ON_RF_ON);
    
    // set last ndef block for fd signaling
    uint8_t last_block = NDEF_BLOCK_START + (ndef_len / NFC_BLOCK_SIZE);
    nfc_set_last_ndef_block(s_config.nfc, last_block);
    
    set_state(NFC_PAIR_NDEF_WRITTEN);
    
    // start timeout timer
    if (s_timeout_timer && s_config.ndef_timeout_ms > 0) {
        xTimerStart(s_timeout_timer, 0);
    }
    
    ESP_LOGI(TAG, "ble pairing ndef written, ready for phone tap");
    return ESP_OK;
}

esp_err_t nfc_pair_clear_ndef(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // stop timeout timer
    if (s_timeout_timer) {
        xTimerStop(s_timeout_timer, 0);
    }
    
    // write default ndef
    uint8_t ndef_buf[128];
    size_t ndef_len = build_default_ndef(ndef_buf, sizeof(ndef_buf));
    
    esp_err_t ret = nfc_write_bytes(s_config.nfc, NDEF_BLOCK_START, ndef_buf, ndef_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to write default ndef: %s", esp_err_to_name(ret));
        return ret;
    }
    
    set_state(NFC_PAIR_IDLE);
    ESP_LOGI(TAG, "ndef cleared, default message written");
    
    return ESP_OK;
}

nfc_pair_state_t nfc_pair_get_state(void)
{
    return s_state;
}

bool nfc_pair_rf_present(void)
{
    if (!s_initialized) {
        return false;
    }
    return nfc_rf_present(s_config.nfc);
}

void nfc_pair_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    
    if (s_timeout_timer) {
        xTimerDelete(s_timeout_timer, 0);
        s_timeout_timer = NULL;
    }
    
    s_initialized = false;
    s_state = NFC_PAIR_IDLE;
    
    ESP_LOGI(TAG, "nfc pairing deinit");
}
