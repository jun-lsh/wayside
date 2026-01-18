/*
 * ble_task.c - BLE UART service with passkey authentication
 * 
 * Supports NFC-triggered pairing flow:
 * 1. ble_init() - Initialize BLE stack (no advertising yet)
 * 2. ble_start_pairing_with_passkey() - Start advertising with passkey auth
 * 3. Phone connects and enters passkey from NFC tag
 * 4. After authentication, UART messages can be exchanged
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_gatt_common_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "ble_task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "name.h"

static const char *TAG = "ble_task";

// Device name
static char s_device_name[NAME_MAX_LEN] = {0};

// BLE configuration
#define EXT_ADV_HANDLE          0
#define BLE_TASK_STACK_SIZE     8192
#define BLE_TASK_PRIORITY       4
#define BLE_QUEUE_SIZE          10
#define BLE_QUEUE_TIMEOUT       pdMS_TO_TICKS(100)

#define PROFILE_NUM             1
#define PROFILE_APP_ID          0
#define SVC_INST_ID             0

// RX buffer for incoming messages
#define RX_BUFFER_SIZE          2048
static uint8_t s_rx_buffer[RX_BUFFER_SIZE];
static int s_rx_buffer_len = 0;

static const char DELIMITER = BLE_MESSAGE_DELIMITER_CHAR;

// Nordic UART Service UUIDs (Little Endian)
static const uint8_t service_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E
};
static const uint8_t char_rx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
};
static const uint8_t char_tx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
};

// Attribute table indices
enum {
    IDX_SVC,
    IDX_CHAR_RX,
    IDX_CHAR_VAL_RX,
    IDX_CHAR_TX,
    IDX_CHAR_VAL_TX,
    IDX_CHAR_CFG_TX,
    BLE_IDX_NB,
};

// Event types for queue
typedef enum {
    BLE_EVT_CONNECT,
    BLE_EVT_DISCONNECT,
    BLE_EVT_DATA_RECV,
    BLE_EVT_MTU_UPDATE,
    BLE_EVT_AUTH_COMPLETE,
} ble_event_id_t;

typedef struct {
    uint8_t *data;
    uint16_t len;
} ble_data_recv_t;

typedef struct {
    ble_event_id_t id;
    union {
        uint16_t conn_id;
        uint16_t mtu;
        bool auth_success;
        ble_data_recv_t recv;
    } info;
} ble_event_t;

// State variables
static uint16_t s_handle_table[BLE_IDX_NB];
static uint16_t s_conn_id = 0;
static esp_gatt_if_t s_gatts_if = 0;
static bool s_is_connected = false;
static bool s_is_paired = false;
static bool s_is_advertising = false;
static uint16_t s_current_mtu = 23;
static QueueHandle_t s_ble_queue = NULL;
static TimerHandle_t s_adv_timeout_timer = NULL;

// Security configuration
static uint32_t s_passkey = 0;
static bool s_use_passkey = false;

// Callbacks
static ble_connection_cb_t s_conn_cb = NULL;
static void *s_conn_cb_arg = NULL;
static ble_auth_cb_t s_auth_cb = NULL;
static void *s_auth_cb_arg = NULL;

// Extended advertising parameters
static esp_ble_gap_ext_adv_params_t s_ext_adv_params = {
    .type = ESP_BLE_GAP_SET_EXT_ADV_PROP_CONNECTABLE,
    .interval_min = 0x20,
    .interval_max = 0x40,
    .channel_map = ADV_CHNL_ALL,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    .primary_phy = ESP_BLE_GAP_PHY_1M,
    .max_skip = 0,
    .secondary_phy = ESP_BLE_GAP_PHY_1M,
    .sid = EXT_ADV_HANDLE,
    .scan_req_notif = false,
};

// Advertising data buffer
static uint8_t s_ext_adv_data[64];
static size_t s_ext_adv_data_len = 0;

// Forward declarations
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void handle_complete_message(const char *message);
static void process_incoming_data(uint8_t *data, uint16_t len);
static esp_err_t start_ext_advertising(void);
static void stop_ext_advertising(void);
static void adv_timeout_callback(TimerHandle_t timer);

// GATT attribute table
#define CHAR_DECLARATION_SIZE (sizeof(uint8_t))

static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t char_prop_notify = ESP_GATT_CHAR_PROP_BIT_NOTIFY;

static const esp_gatts_attr_db_t s_gatt_db[BLE_IDX_NB] = {
    [IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
         sizeof(service_uuid), sizeof(service_uuid), (uint8_t *)service_uuid}
    },
    [IDX_CHAR_RX] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
         CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_write}
    },
    [IDX_CHAR_VAL_RX] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)char_rx_uuid, ESP_GATT_PERM_WRITE,
         RX_BUFFER_SIZE, 0, NULL}
    },
    [IDX_CHAR_TX] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
         CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_notify}
    },
    [IDX_CHAR_VAL_TX] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)char_tx_uuid, ESP_GATT_PERM_READ,
         RX_BUFFER_SIZE, 0, NULL}
    },
    [IDX_CHAR_CFG_TX] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(uint16_t), 0, NULL}
    },
};

// === Message Handling ===

static int hex_to_bytes(const char *hex, uint8_t *out, int max_len)
{
    int hex_len = strlen(hex);
    if (hex_len % 2 != 0) return -1;
    
    int byte_len = hex_len / 2;
    if (byte_len > max_len) return -1;
    
    for (int i = 0; i < byte_len; i++) {
        char byte_str[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        char *endptr;
        long val = strtol(byte_str, &endptr, 16);
        if (*endptr != '\0') return -1;
        out[i] = (uint8_t)val;
    }
    return byte_len;
}

/**
 * Handle a complete message from the phone
 * 
 * Message protocol (after BLE pairing is complete):
 * - PUBKEY:<base64_key> - Store RSA public key
 * - BITMASK:<bits>:<hex>[:threshold] - Store interest bitmask
 * - ENC_URL:<data> - Encrypted URL to relay
 * - ping - Respond with pong
 */
static void handle_complete_message(const char *message)
{
    ESP_LOGI(TAG, "RX: %s", message);
    
    // PUBKEY command - store RSA public key
    if (strncmp(message, "PUBKEY:", 7) == 0) {
        const char *public_key = message + 7;
        ESP_LOGI(TAG, "Received public key (%d bytes)", strlen(public_key));
        
        // Store in NVS
        nvs_handle_t handle;
        if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_str(handle, "pubkey", public_key);
            nvs_commit(handle);
            nvs_close(handle);
        }
        
        ble_send_message("PUBKEY_OK" BLE_MESSAGE_DELIMITER_STR);
        return;
    }
    
    // BITMASK command - store interest bitmask  
    if (strncmp(message, "BITMASK:", 8) == 0) {
        const char *after_prefix = message + 8;
        const char *colon = strchr(after_prefix, ':');
        if (!colon) {
            ble_send_message("BITMASK_ERR:FORMAT" BLE_MESSAGE_DELIMITER_STR);
            return;
        }
        
        int bits = atoi(after_prefix);
        if (bits <= 0 || bits > 2048) {
            ble_send_message("BITMASK_ERR:LEN" BLE_MESSAGE_DELIMITER_STR);
            return;
        }
        
        int expected_bytes = (bits + 7) / 8;
        const char *hex_data = colon + 1;
        
        // Parse optional threshold
        uint8_t threshold = 50;
        int hex_len = strlen(hex_data);
        const char *threshold_colon = strrchr(hex_data, ':');
        if (threshold_colon) {
            int thresh = atoi(threshold_colon + 1);
            if (thresh >= 0 && thresh <= 100) {
                threshold = (uint8_t)thresh;
            }
            hex_len = threshold_colon - hex_data;
        }
        
        uint8_t *binary = malloc(expected_bytes);
        if (!binary) {
            ble_send_message("BITMASK_ERR:MEM" BLE_MESSAGE_DELIMITER_STR);
            return;
        }
        
        char *hex_copy = malloc(hex_len + 1);
        if (!hex_copy) {
            free(binary);
            ble_send_message("BITMASK_ERR:MEM" BLE_MESSAGE_DELIMITER_STR);
            return;
        }
        memcpy(hex_copy, hex_data, hex_len);
        hex_copy[hex_len] = '\0';
        
        int actual_bytes = hex_to_bytes(hex_copy, binary, expected_bytes);
        free(hex_copy);
        
        if (actual_bytes != expected_bytes) {
            free(binary);
            ble_send_message("BITMASK_ERR:DATA" BLE_MESSAGE_DELIMITER_STR);
            return;
        }
        
        // Store in NVS
        nvs_handle_t handle;
        if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_blob(handle, "bitmask", binary, actual_bytes);
            nvs_set_u8(handle, "bitmask_thr", threshold);
            nvs_commit(handle);
            nvs_close(handle);
        }
        
        free(binary);
        ble_send_message("BITMASK_OK" BLE_MESSAGE_DELIMITER_STR);
        return;
    }
    
    // ENC_URL command
    if (strncmp(message, "ENC_URL:", 8) == 0) {
        ESP_LOGI(TAG, "Received encrypted URL");
        ble_send_message("ENC_URL_OK" BLE_MESSAGE_DELIMITER_STR);
        return;
    }
    
    // ping command
    if (strcmp(message, "ping") == 0) {
        ble_send_message("pong" BLE_MESSAGE_DELIMITER_STR);
        return;
    }
    
    ESP_LOGW(TAG, "Unknown command: %s", message);
}

static void process_incoming_data(uint8_t *data, uint16_t len)
{
    if (s_rx_buffer_len + len > RX_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Buffer overflow, resetting");
        s_rx_buffer_len = 0;
        return;
    }
    
    memcpy(s_rx_buffer + s_rx_buffer_len, data, len);
    s_rx_buffer_len += len;
    
    // Scan for delimiter
    for (int i = 0; i < s_rx_buffer_len; i++) {
        if (s_rx_buffer[i] == DELIMITER) {
            s_rx_buffer[i] = '\0';
            handle_complete_message((char *)s_rx_buffer);
            
            int leftover = s_rx_buffer_len - (i + 1);
            if (leftover > 0) {
                memmove(s_rx_buffer, s_rx_buffer + i + 1, leftover);
                s_rx_buffer_len = leftover;
                i = -1;
            } else {
                s_rx_buffer_len = 0;
            }
        }
    }
}

// === Advertising ===

static void build_ext_adv_data(void)
{
    size_t pos = 0;
    
    // Flags
    s_ext_adv_data[pos++] = 2;
    s_ext_adv_data[pos++] = ESP_BLE_AD_TYPE_FLAG;
    s_ext_adv_data[pos++] = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
    
    // Complete local name
    size_t name_len = strlen(s_device_name);
    if (name_len > 20) name_len = 20;
    s_ext_adv_data[pos++] = name_len + 1;
    s_ext_adv_data[pos++] = ESP_BLE_AD_TYPE_NAME_CMPL;
    memcpy(&s_ext_adv_data[pos], s_device_name, name_len);
    pos += name_len;
    
    // 128-bit service UUID
    s_ext_adv_data[pos++] = 17;
    s_ext_adv_data[pos++] = ESP_BLE_AD_TYPE_128SRV_CMPL;
    memcpy(&s_ext_adv_data[pos], service_uuid, 16);
    pos += 16;
    
    // TX power
    s_ext_adv_data[pos++] = 2;
    s_ext_adv_data[pos++] = ESP_BLE_AD_TYPE_TX_PWR;
    s_ext_adv_data[pos++] = 0x00;
    
    s_ext_adv_data_len = pos;
}

static esp_err_t configure_security(void)
{
    esp_ble_auth_req_t auth_req;
    esp_ble_io_cap_t io_cap;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t oob_support = ESP_BLE_OOB_DISABLE;
    
    if (s_use_passkey) {
        // Passkey entry mode
        auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
        io_cap = ESP_IO_CAP_OUT;  // Display only (we show passkey)
        
        // Set static passkey
        uint32_t passkey = s_passkey;
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(uint32_t));
        
        ESP_LOGI(TAG, "Security: Passkey mode (key=%06lu)", (unsigned long)s_passkey);
    } else {
        // Just Works mode (no passkey)
        auth_req = ESP_LE_AUTH_REQ_SC_BOND;
        io_cap = ESP_IO_CAP_NONE;
        
        ESP_LOGI(TAG, "Security: Just Works mode");
    }
    
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_OOB_SUPPORT, &oob_support, sizeof(uint8_t));
    
    return ESP_OK;
}

static esp_err_t start_ext_advertising(void)
{
    if (s_is_advertising) {
        ESP_LOGW(TAG, "Already advertising");
        return ESP_OK;
    }
    
    esp_err_t ret = esp_ble_gap_ext_adv_set_params(EXT_ADV_HANDLE, &s_ext_adv_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set ext adv params: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;  // Advertising starts after params complete event
}

static void stop_ext_advertising(void)
{
    if (!s_is_advertising) return;
    
    esp_ble_gap_ext_adv_stop(1, &(uint8_t){EXT_ADV_HANDLE});
    s_is_advertising = false;
    ESP_LOGI(TAG, "Advertising stopped");
}

static void adv_timeout_callback(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Advertising timeout");
    stop_ext_advertising();
}

// === GAP Event Handler ===

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_EXT_ADV_SET_PARAMS_COMPLETE_EVT:
            if (param->ext_adv_set_params.status == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Ext adv params set, configuring data");
                build_ext_adv_data();
                esp_ble_gap_config_ext_adv_data_raw(EXT_ADV_HANDLE, s_ext_adv_data_len, s_ext_adv_data);
            }
            break;
            
        case ESP_GAP_BLE_EXT_ADV_DATA_SET_COMPLETE_EVT:
            if (param->ext_adv_data_set.status == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Ext adv data set, starting advertising");
                esp_ble_gap_ext_adv_start(1, &(esp_ble_gap_ext_adv_t){
                    .instance = EXT_ADV_HANDLE,
                    .duration = 0,
                    .max_events = 0,
                });
            }
            break;
            
        case ESP_GAP_BLE_EXT_ADV_START_COMPLETE_EVT:
            if (param->ext_adv_start.status == ESP_BT_STATUS_SUCCESS) {
                s_is_advertising = true;
                ESP_LOGI(TAG, "Advertising started");
            }
            break;
            
        case ESP_GAP_BLE_EXT_ADV_STOP_COMPLETE_EVT:
            s_is_advertising = false;
            break;
            
        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            // Display passkey (we already know it from NFC)
            ESP_LOGI(TAG, "Passkey notify: %06lu", (unsigned long)param->ble_security.key_notif.passkey);
            break;
            
        case ESP_GAP_BLE_NC_REQ_EVT:
            // Numeric comparison - auto accept
            ESP_LOGI(TAG, "Numeric comparison: %06lu", (unsigned long)param->ble_security.key_notif.passkey);
            esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
            break;
            
        case ESP_GAP_BLE_SEC_REQ_EVT:
            // Security request from peer
            ESP_LOGI(TAG, "Security request");
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;
            
        case ESP_GAP_BLE_AUTH_CMPL_EVT: {
            esp_bd_addr_t bd_addr;
            memcpy(bd_addr, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
            
            if (param->ble_security.auth_cmpl.success) {
                ESP_LOGI(TAG, "Authentication SUCCESS");
                s_is_paired = true;
                
                // Queue event
                ble_event_t evt = {
                    .id = BLE_EVT_AUTH_COMPLETE,
                    .info.auth_success = true,
                };
                xQueueSend(s_ble_queue, &evt, BLE_QUEUE_TIMEOUT);
            } else {
                ESP_LOGW(TAG, "Authentication FAILED (reason=%d)", 
                         param->ble_security.auth_cmpl.fail_reason);
                s_is_paired = false;
                
                ble_event_t evt = {
                    .id = BLE_EVT_AUTH_COMPLETE,
                    .info.auth_success = false,
                };
                xQueueSend(s_ble_queue, &evt, BLE_QUEUE_TIMEOUT);
            }
            break;
        }
            
        default:
            break;
    }
}

// === GATTS Event Handler ===

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    ble_event_t evt;
    
    switch (event) {
        case ESP_GATTS_REG_EVT:
            if (param->reg.status == ESP_GATT_OK) {
                s_gatts_if = gatts_if;
                esp_ble_gap_set_device_name(s_device_name);
                esp_ble_gatts_create_attr_tab(s_gatt_db, gatts_if, BLE_IDX_NB, SVC_INST_ID);
            }
            break;
            
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status == ESP_GATT_OK && 
                param->add_attr_tab.num_handle == BLE_IDX_NB) {
                memcpy(s_handle_table, param->add_attr_tab.handles, sizeof(s_handle_table));
                esp_ble_gatts_start_service(s_handle_table[IDX_SVC]);
            }
            break;
            
        case ESP_GATTS_START_EVT:
            ESP_LOGI(TAG, "GATT service started");
            break;
            
        case ESP_GATTS_CONNECT_EVT:
            ESP_LOGI(TAG, "Device connected (conn_id=%d)", param->connect.conn_id);
            evt.id = BLE_EVT_CONNECT;
            evt.info.conn_id = param->connect.conn_id;
            xQueueSend(s_ble_queue, &evt, BLE_QUEUE_TIMEOUT);
            
            // Request MTU exchange
            //esp_ble_gattc_send_mtu_req(gatts_if, param->connect.conn_id);
            
            // Initiate security if using passkey
            if (s_use_passkey) {
                esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
            }
            break;
            
        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(TAG, "Device disconnected");
            evt.id = BLE_EVT_DISCONNECT;
            xQueueSend(s_ble_queue, &evt, BLE_QUEUE_TIMEOUT);
            
            // Restart advertising if not timeout
            if (s_adv_timeout_timer == NULL || xTimerIsTimerActive(s_adv_timeout_timer)) {
                start_ext_advertising();
            }
            break;
            
        case ESP_GATTS_MTU_EVT:
            evt.id = BLE_EVT_MTU_UPDATE;
            evt.info.mtu = param->mtu.mtu;
            xQueueSend(s_ble_queue, &evt, BLE_QUEUE_TIMEOUT);
            break;
            
        case ESP_GATTS_WRITE_EVT:
            if (param->write.handle == s_handle_table[IDX_CHAR_VAL_RX]) {
                uint8_t *data_copy = malloc(param->write.len);
                if (data_copy) {
                    memcpy(data_copy, param->write.value, param->write.len);
                    evt.id = BLE_EVT_DATA_RECV;
                    evt.info.recv.data = data_copy;
                    evt.info.recv.len = param->write.len;
                    if (xQueueSend(s_ble_queue, &evt, BLE_QUEUE_TIMEOUT) != pdTRUE) {
                        free(data_copy);
                    }
                }
            }
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, NULL);
            }
            break;
            
        default:
            break;
    }
}

// === BLE Task ===

static void ble_task(void *pvParameter)
{
    ble_event_t evt;
    
    ESP_LOGI(TAG, "BLE task started");
    
    while (1) {
        if (xQueueReceive(s_ble_queue, &evt, portMAX_DELAY) == pdTRUE) {
            switch (evt.id) {
                case BLE_EVT_CONNECT:
                    s_conn_id = evt.info.conn_id;
                    s_is_connected = true;
                    s_is_paired = false;
                    if (s_conn_cb) s_conn_cb(true, s_conn_cb_arg);
                    break;
                    
                case BLE_EVT_DISCONNECT:
                    s_is_connected = false;
                    s_is_paired = false;
                    s_rx_buffer_len = 0;
                    if (s_conn_cb) s_conn_cb(false, s_conn_cb_arg);
                    break;
                    
                case BLE_EVT_MTU_UPDATE:
                    s_current_mtu = evt.info.mtu;
                    ESP_LOGI(TAG, "MTU updated to %d", evt.info.mtu);
                    break;
                    
                case BLE_EVT_DATA_RECV:
                    process_incoming_data(evt.info.recv.data, evt.info.recv.len);
                    free(evt.info.recv.data);
                    break;
                    
                case BLE_EVT_AUTH_COMPLETE:
                    if (s_auth_cb) s_auth_cb(evt.info.auth_success, s_auth_cb_arg);
                    break;
                    
                default:
                    break;
            }
        }
    }
}

// === Public API ===

esp_err_t ble_init(void)
{
    esp_err_t ret;
    
    // Get device name
    ret = name_get(0, s_device_name, sizeof(s_device_name));
    if (ret != ESP_OK) {
        snprintf(s_device_name, sizeof(s_device_name), "ESP-BLE");
    }
    ESP_LOGI(TAG, "Device name: %s", s_device_name);
    
    // Create event queue
    s_ble_queue = xQueueCreate(BLE_QUEUE_SIZE, sizeof(ble_event_t));
    if (!s_ble_queue) {
        ESP_LOGE(TAG, "Failed to create queue");
        return ESP_FAIL;
    }
    
    // Initialize BT controller
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize Bluedroid
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Register callbacks
    esp_ble_gatts_register_callback(gatts_event_handler);
    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gatts_app_register(PROFILE_APP_ID);
    
    // Set MTU
    esp_ble_gatt_set_local_mtu(247);
    
    // Create BLE task
    xTaskCreate(ble_task, "ble_task", BLE_TASK_STACK_SIZE, NULL, BLE_TASK_PRIORITY, NULL);
    
    ESP_LOGI(TAG, "BLE initialized (not advertising yet)");
    return ESP_OK;
}

esp_err_t ble_start_pairing_with_passkey(uint32_t passkey, uint32_t timeout_sec)
{
    ESP_LOGI(TAG, "Starting pairing with passkey %06lu (timeout=%lu sec)", 
             (unsigned long)passkey, (unsigned long)timeout_sec);
    
    s_passkey = passkey;
    s_use_passkey = true;
    
    // Configure security
    configure_security();
    
    // Setup timeout timer
    if (timeout_sec > 0) {
        if (s_adv_timeout_timer) {
            xTimerDelete(s_adv_timeout_timer, 0);
        }
        s_adv_timeout_timer = xTimerCreate(
            "adv_timeout",
            pdMS_TO_TICKS(timeout_sec * 1000),
            pdFALSE,
            NULL,
            adv_timeout_callback
        );
        if (s_adv_timeout_timer) {
            xTimerStart(s_adv_timeout_timer, 0);
        }
    }
    
    return start_ext_advertising();
}

esp_err_t ble_start_pairing(uint32_t timeout_sec)
{
    ESP_LOGI(TAG, "Starting pairing (Just Works, timeout=%lu sec)", (unsigned long)timeout_sec);
    
    s_use_passkey = false;
    
    configure_security();
    
    if (timeout_sec > 0) {
        if (s_adv_timeout_timer) {
            xTimerDelete(s_adv_timeout_timer, 0);
        }
        s_adv_timeout_timer = xTimerCreate(
            "adv_timeout",
            pdMS_TO_TICKS(timeout_sec * 1000),
            pdFALSE,
            NULL,
            adv_timeout_callback
        );
        if (s_adv_timeout_timer) {
            xTimerStart(s_adv_timeout_timer, 0);
        }
    }
    
    return start_ext_advertising();
}

void ble_stop_advertising(void)
{
    if (s_adv_timeout_timer) {
        xTimerStop(s_adv_timeout_timer, 0);
    }
    stop_ext_advertising();
}

void ble_send_message(const char *message)
{
    if (!s_is_connected || !message) return;
    
    size_t len = strlen(message);
    if (len == 0) return;
    
    uint16_t max_chunk = s_current_mtu - 3;
    if (max_chunk < 20) max_chunk = 20;
    
    size_t offset = 0;
    while (offset < len) {
        size_t chunk_len = len - offset;
        if (chunk_len > max_chunk) chunk_len = max_chunk;
        
        esp_err_t ret = esp_ble_gatts_send_indicate(
            s_gatts_if, s_conn_id,
            s_handle_table[IDX_CHAR_VAL_TX],
            chunk_len,
            (uint8_t *)(message + offset),
            false
        );
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Send failed: %s", esp_err_to_name(ret));
            return;
        }
        
        offset += chunk_len;
        if (offset < len) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

bool ble_is_connected(void)
{
    return s_is_connected;
}

bool ble_is_paired(void)
{
    return s_is_paired;
}

esp_err_t ble_get_mac(uint8_t *mac)
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    const uint8_t *addr = esp_bt_dev_get_address();
    if (!addr) return ESP_ERR_INVALID_STATE;
    memcpy(mac, addr, 6);
    return ESP_OK;
}

const char *ble_get_device_name(void)
{
    return s_device_name;
}

esp_err_t ble_set_adv_phy(esp_ble_gap_phy_t primary_phy, esp_ble_gap_phy_t secondary_phy)
{
    s_ext_adv_params.primary_phy = primary_phy;
    s_ext_adv_params.secondary_phy = secondary_phy;
    return ESP_OK;
}

esp_err_t ble_enable_long_range(bool enable)
{
    if (enable) {
        return ble_set_adv_phy(ESP_BLE_GAP_PHY_CODED, ESP_BLE_GAP_PHY_CODED);
    }
    return ble_set_adv_phy(ESP_BLE_GAP_PHY_1M, ESP_BLE_GAP_PHY_1M);
}

esp_err_t ble_disconnect(void)
{
    if (!s_is_connected) return ESP_OK;
    return esp_ble_gatts_close(s_gatts_if, s_conn_id);
}

void ble_set_connection_callback(ble_connection_cb_t cb, void *arg)
{
    s_conn_cb = cb;
    s_conn_cb_arg = arg;
}

void ble_set_auth_callback(ble_auth_cb_t cb, void *arg)
{
    s_auth_cb = cb;
    s_auth_cb_arg = arg;
}
