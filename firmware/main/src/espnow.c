#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "espnow.h"
#include "pairing.h"

#define ESPNOW_MAXDELAY 512

static const char *TAG = "espnow";

static QueueHandle_t s_espnow_queue = NULL;

const uint8_t espnow_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static pairing_ctx_t s_pairing_ctx;

void espnow_set_config_key(const char *key) {
    if (s_espnow_queue == NULL || key == NULL) return;

    espnow_event_t evt;
    evt.id = ESPNOW_SET_KEY;
    // Safety copy
    strncpy(evt.info.set_key.key, key, PAIRING_KEY_MAX_LEN - 1);
    evt.info.set_key.key[PAIRING_KEY_MAX_LEN - 1] = '\0';
    
    xQueueSend(s_espnow_queue, &evt, portMAX_DELAY);
}

/* ESPNOW sending callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    espnow_event_t evt;
    espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

    if (tx_info == NULL) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    evt.id = ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, tx_info->des_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send send queue fail");
    }
}

/* ESPNOW receiving callback function is called in WiFi task. */
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    espnow_event_t evt;
    espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
    uint8_t *mac_addr = recv_info->src_addr;
    uint8_t *des_addr = recv_info->des_addr;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    int8_t rssi = recv_info->rx_ctrl->rssi;
    int8_t noise_floor = recv_info->rx_ctrl->noise_floor;

    float distance_m = powf(10.0f, (float)(ESPNOW_TX_POWER_DBM - rssi) / (10.0f * ESPNOW_PATH_LOSS_EXP));

    const char *zone;
    if (rssi >= RSSI_ZONE_VERY_CLOSE) {
        zone = "VERY_CLOSE";
    } else if (rssi >= RSSI_ZONE_CLOSE) {
        zone = "CLOSE";
    } else if (rssi >= RSSI_ZONE_MEDIUM) {
        zone = "MEDIUM";
    } else if (rssi >= RSSI_ZONE_FAR) {
        zone = "FAR";
    } else {
        zone = "EDGE";
    }

    if (IS_BROADCAST_ADDR(des_addr)) {
        ESP_LOGI(TAG, "Recv broadcast from "MACSTR" | RSSI: %d dBm | Dist: %.1fm | Zone: %s",
                 MAC2STR(mac_addr), rssi, distance_m, zone);
    } else {
        ESP_LOGI(TAG, "Recv unicast from "MACSTR" | RSSI: %d dBm | Dist: %.1fm | Zone: %s",
                 MAC2STR(mac_addr), rssi, distance_m, zone);
    }

    evt.id = ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->rssi = rssi;
    recv_cb->noise_floor = noise_floor;
    recv_cb->data = malloc(len);
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG, "Malloc receive data fail");
        return;
    }
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send receive queue fail");
        free(recv_cb->data);
    }
}

static void espnow_task(void *pvParameter)
{
    espnow_event_t evt;

    ESP_LOGI(TAG, "ESP-NOW task started. Broadcasting DISABLED until key received.");

    while (1) {
        /* Use timeout so pairing_tick can run periodically */
        if (xQueueReceive(s_espnow_queue, &evt, pdMS_TO_TICKS(PAIRING_REBROADCAST_MS)) == pdTRUE) {
            switch (evt.id) {
                case ESPNOW_SEND_CB:
                {
                    espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
                    ESP_LOGD(TAG, "Send to " MACSTR " status: %s", 
                             MAC2STR(send_cb->mac_addr),
                             send_cb->status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
                    break;
                }
                case ESPNOW_RECV_CB:
                {
                    espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

                    pairing_handle_recv(&s_pairing_ctx, recv_cb->mac_addr, 
                                        recv_cb->data, recv_cb->data_len, recv_cb->rssi);

                    free(recv_cb->data);
                    break;
                }
                case ESPNOW_SET_KEY:
                    ESP_LOGI(TAG, "Applying Public Key to Pairing Context");
                    pairing_set_config(&s_pairing_ctx, evt.info.set_key.key);
                    break;
                default:
                    ESP_LOGE(TAG, "Unknown event id: %d", evt.id);
                    break;
            }
        }

        /* Run pairing tick for timeouts and periodic broadcasts */
        pairing_tick(&s_pairing_ctx);
    }
}

esp_err_t espnow_init(void)
{
    s_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
    if (s_espnow_queue == NULL) {
        ESP_LOGE(TAG, "Create queue fail");
        return ESP_FAIL;
    }

    /* Initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(espnow_send_cb) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(espnow_recv_cb) );
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
    ESP_ERROR_CHECK( esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW) );
    ESP_ERROR_CHECK( esp_wifi_connectionless_module_set_wake_interval(CONFIG_ESPNOW_WAKE_INTERVAL) );
#endif
    /* Set primary master key. */
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );

    /* Add broadcast peer information to peer list. */
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
        ESP_LOGE(TAG, "Malloc peer information fail");
        vQueueDelete(s_espnow_queue);
        s_espnow_queue = NULL;
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    memcpy(peer->peer_addr, espnow_broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK( esp_now_add_peer(peer) );
    free(peer);

    /* Initialize pairing state machine */
    esp_err_t pairing_ret = pairing_init(&s_pairing_ctx);
    if (pairing_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize pairing: %s", esp_err_to_name(pairing_ret));
        return pairing_ret;
    }

    /* Start ESP-NOW task */
    xTaskCreate(espnow_task, "espnow_task", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "ESP-NOW initialized");
    return ESP_OK;
}
