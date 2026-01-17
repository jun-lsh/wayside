#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "ble_task.h"

static const char *TAG = "ble_task";

#define BLE_DEVICE_NAME             "ESP-BLE-1"
#define BLE_TASK_STACK_SIZE         8192
#define BLE_TASK_PRIORITY           4
#define BLE_QUEUE_SIZE              10
#define BLE_QUEUE_TIMEOUT           pdMS_TO_TICKS(100)

#define PROFILE_NUM                 1
#define PROFILE_APP_ID              0
#define SVC_INST_ID                 0

// Buffer for reassembling incoming chunks
#define RX_BUFFER_SIZE              2048
static uint8_t s_rx_buffer[RX_BUFFER_SIZE];
static int s_rx_buffer_len = 0;

// Message delimiter (matches React Native config.messageDelimiter)
static const char DELIMITER = '\n';

// --- NORDIC UART SERVICE UUIDS (Little Endian) ---
// Service: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
static const uint8_t service_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E
};
// RX Characteristic (Write): 6e400002-b5a3-f393-e0a9-e50e24dcca9e
static const uint8_t char_rx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
};
// TX Characteristic (Notify): 6e400003-b5a3-f393-e0a9-e50e24dcca9e
static const uint8_t char_tx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
};

// --- ATTRIBUTE TABLE INDICES ---
enum {
    IDX_SVC,
    IDX_CHAR_RX,
    IDX_CHAR_VAL_RX,
    IDX_CHAR_TX,
    IDX_CHAR_VAL_TX,
    IDX_CHAR_CFG_TX,    // CCCD for notifications
    BLE_IDX_NB,
};

// --- BLE EVENT TYPES FOR QUEUE ---
typedef enum {
    BLE_EVT_CONNECT,
    BLE_EVT_DISCONNECT,
    BLE_EVT_DATA_RECV,
    BLE_EVT_MTU_UPDATE,
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
        ble_data_recv_t recv;
    } info;
} ble_event_t;

// --- STATE VARIABLES ---
static uint16_t s_handle_table[BLE_IDX_NB];
static uint16_t s_conn_id = 0;
static esp_gatt_if_t s_gatts_if = 0;
static bool s_is_connected = false;
static QueueHandle_t s_ble_queue = NULL;

// --- ADVERTISING CONFIG ---
static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(service_uuid),
    .p_service_uuid = (uint8_t *)service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// --- GATT ATTRIBUTE TABLE ---
#define CHAR_DECLARATION_SIZE   (sizeof(uint8_t))

static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t char_prop_notify = ESP_GATT_CHAR_PROP_BIT_NOTIFY;

static const esp_gatts_attr_db_t s_gatt_db[BLE_IDX_NB] = {
    // Service Declaration
    [IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
         sizeof(service_uuid), sizeof(service_uuid), (uint8_t *)service_uuid}
    },

    // RX Characteristic Declaration (Phone -> ESP)
    [IDX_CHAR_RX] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
         CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_write}
    },
    // RX Characteristic Value
    [IDX_CHAR_VAL_RX] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)char_rx_uuid, ESP_GATT_PERM_WRITE,
         RX_BUFFER_SIZE, 0, NULL}
    },

    // TX Characteristic Declaration (ESP -> Phone)
    [IDX_CHAR_TX] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
         CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_notify}
    },
    // TX Characteristic Value
    [IDX_CHAR_VAL_TX] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)char_tx_uuid, ESP_GATT_PERM_READ,
         RX_BUFFER_SIZE, 0, NULL}
    },
    // TX CCCD (Client Characteristic Configuration Descriptor)
    [IDX_CHAR_CFG_TX] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(uint16_t), 0, NULL}
    },
};

// --- INTERNAL FUNCTIONS ---

/**
 * Process a complete message received from the Android device.
 * Add your application logic here.
 */
static void handle_complete_message(const char *message)
{
    ESP_LOGI(TAG, "RX Message: %s", message);

    // Example: Echo "pong" when "ping" is received
    if (strcmp(message, "ping") == 0) {
        ble_send_message("pong\n");
    }

    // TODO: Add your custom message handling logic here
}

/**
 * Process incoming data chunks and reassemble until delimiter is found.
 */
static void process_incoming_data(uint8_t *data, uint16_t len)
{
    if (s_rx_buffer_len + len > RX_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Buffer overflow! Resetting.");
        s_rx_buffer_len = 0;
        return;
    }

    memcpy(s_rx_buffer + s_rx_buffer_len, data, len);
    s_rx_buffer_len += len;

    // Scan for delimiter
    for (int i = 0; i < s_rx_buffer_len; i++) {
        if (s_rx_buffer[i] == DELIMITER) {
            // Null-terminate at delimiter
            s_rx_buffer[i] = '\0';

            // Process complete message
            handle_complete_message((char *)s_rx_buffer);

            // Handle any leftover data after delimiter
            int leftover = s_rx_buffer_len - (i + 1);
            if (leftover > 0) {
                memmove(s_rx_buffer, s_rx_buffer + i + 1, leftover);
                s_rx_buffer_len = leftover;
                i = -1; // Restart scan
            } else {
                s_rx_buffer_len = 0;
            }
        }
    }
}

// --- GAP EVENT HANDLER ---
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            ESP_LOGI(TAG, "Advertising data set, starting advertising");
            esp_ble_gap_start_advertising(&s_adv_params);
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising start failed: %d", param->adv_start_cmpl.status);
            } else {
                ESP_LOGI(TAG, "Advertising started");
            }
            break;

        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGI(TAG, "Connection params updated: interval=%d, latency=%d, timeout=%d",
                     param->update_conn_params.conn_int,
                     param->update_conn_params.latency,
                     param->update_conn_params.timeout);
            break;

        default:
            break;
    }
}

// --- GATTS EVENT HANDLER ---
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    ble_event_t evt;

    switch (event) {
        case ESP_GATTS_REG_EVT:
            if (param->reg.status == ESP_GATT_OK) {
                ESP_LOGI(TAG, "GATT server registered, app_id=%d", param->reg.app_id);
                s_gatts_if = gatts_if;
                esp_ble_gap_set_device_name(BLE_DEVICE_NAME);
                esp_ble_gap_config_adv_data(&s_adv_data);
                esp_ble_gatts_create_attr_tab(s_gatt_db, gatts_if, BLE_IDX_NB, SVC_INST_ID);
            } else {
                ESP_LOGE(TAG, "GATT server registration failed: %d", param->reg.status);
            }
            break;

        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Create attribute table failed: 0x%x", param->add_attr_tab.status);
            } else if (param->add_attr_tab.num_handle != BLE_IDX_NB) {
                ESP_LOGE(TAG, "Attribute table abnormal, handles=%d (expected %d)",
                         param->add_attr_tab.num_handle, BLE_IDX_NB);
            } else {
                memcpy(s_handle_table, param->add_attr_tab.handles, sizeof(s_handle_table));
                esp_ble_gatts_start_service(s_handle_table[IDX_SVC]);
                ESP_LOGI(TAG, "UART service started");
            }
            break;

        case ESP_GATTS_CONNECT_EVT:
            ESP_LOGI(TAG, "Device connected, conn_id=%d", param->connect.conn_id);
            evt.id = BLE_EVT_CONNECT;
            evt.info.conn_id = param->connect.conn_id;
            xQueueSend(s_ble_queue, &evt, BLE_QUEUE_TIMEOUT);
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(TAG, "Device disconnected, reason=0x%x", param->disconnect.reason);
            evt.id = BLE_EVT_DISCONNECT;
            xQueueSend(s_ble_queue, &evt, BLE_QUEUE_TIMEOUT);
            // Restart advertising
            esp_ble_gap_start_advertising(&s_adv_params);
            break;

        case ESP_GATTS_MTU_EVT:
            ESP_LOGI(TAG, "MTU negotiated: %d", param->mtu.mtu);
            evt.id = BLE_EVT_MTU_UPDATE;
            evt.info.mtu = param->mtu.mtu;
            xQueueSend(s_ble_queue, &evt, BLE_QUEUE_TIMEOUT);
            break;

        case ESP_GATTS_WRITE_EVT:
            if (!param->write.is_prep) {
                // Check if write is to RX characteristic
                if (s_handle_table[IDX_CHAR_VAL_RX] == param->write.handle) {
                    // Allocate and copy data for queue
                    uint8_t *data_copy = malloc(param->write.len);
                    if (data_copy != NULL) {
                        memcpy(data_copy, param->write.value, param->write.len);
                        evt.id = BLE_EVT_DATA_RECV;
                        evt.info.recv.data = data_copy;
                        evt.info.recv.len = param->write.len;
                        if (xQueueSend(s_ble_queue, &evt, BLE_QUEUE_TIMEOUT) != pdTRUE) {
                            free(data_copy);
                            ESP_LOGW(TAG, "Failed to queue received data");
                        }
                    } else {
                        ESP_LOGE(TAG, "Failed to allocate memory for received data");
                    }
                }
            }
            // Send response if required
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                            param->write.trans_id, ESP_GATT_OK, NULL);
            }
            break;

        default:
            break;
    }
}

// --- BLE TASK ---
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
                    ESP_LOGI(TAG, "Connection established, conn_id=%d", s_conn_id);
                    break;

                case BLE_EVT_DISCONNECT:
                    s_is_connected = false;
                    s_rx_buffer_len = 0; // Clear receive buffer
                    ESP_LOGI(TAG, "Connection closed");
                    break;

                case BLE_EVT_MTU_UPDATE:
                    ESP_LOGI(TAG, "MTU updated to %d", evt.info.mtu);
                    break;

                case BLE_EVT_DATA_RECV:
                    ESP_LOGI(TAG, "received data %d, adadtadausdhauisdhnaiushdnuisahdu", evt.info.recv.len);
                    process_incoming_data(evt.info.recv.data, evt.info.recv.len);
                    free(evt.info.recv.data);
                    break;

                default:
                    ESP_LOGW(TAG, "Unknown event id: %d", evt.id);
                    break;
            }
        }
    }
}

// --- PUBLIC API ---

esp_err_t ble_init(void)
{
    esp_err_t ret;

    // Create event queue
    s_ble_queue = xQueueCreate(BLE_QUEUE_SIZE, sizeof(ble_event_t));
    if (s_ble_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create BLE event queue");
        return ESP_FAIL;
    }

    // Release classic BT memory (we only use BLE)
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    // Initialize BT controller
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

    // Initialize Bluedroid stack
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
    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTS register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register GATT application
    ret = esp_ble_gatts_app_register(PROFILE_APP_ID);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTS app register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set local MTU (max 517)
    // esp_ble_gatt_set_local_mtu(517);

    // Create BLE task
    BaseType_t task_ret = xTaskCreate(ble_task, "ble_task", BLE_TASK_STACK_SIZE,
                                       NULL, BLE_TASK_PRIORITY, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create BLE task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE initialized successfully");
    return ESP_OK;
}

void ble_send_message(const char *message)
{
    if (!s_is_connected) {
        ESP_LOGW(TAG, "Cannot send: No device connected");
        return;
    }

    if (message == NULL) {
        ESP_LOGW(TAG, "Cannot send: NULL message");
        return;
    }

    size_t len = strlen(message);
    if (len == 0) {
        return;
    }

    esp_err_t ret = esp_ble_gatts_send_indicate(
        s_gatts_if,
        s_conn_id,
        s_handle_table[IDX_CHAR_VAL_TX],
        len,
        (uint8_t *)message,
        false  // false = notification (no ACK), true = indication (requires ACK)
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send notification: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Sent: %s", message);
    }
}

bool ble_is_connected(void)
{
    return s_is_connected;
}
