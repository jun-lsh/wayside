#ifndef ESPNOW_H
#define ESPNOW_H

#include "esp_now.h"
#include "esp_err.h"

/* ESPNOW can work in both station and softap mode. It is configured in menuconfig. */
#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF   WIFI_IF_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF   WIFI_IF_AP
#endif

#define ESPNOW_QUEUE_SIZE           6

#define IS_BROADCAST_ADDR(addr) (memcmp(addr, espnow_broadcast_mac, ESP_NOW_ETH_ALEN) == 0)

/*
 * RSSI-based distance estimation using log-distance path loss model:
 *
 *   Distance (m) = 10 ^ ((TxPower - RSSI) / (10 * n))
 *
 * TxPower = calibrated RSSI at 1 meter (reference point)
 * RSSI    = received signal strength from rx_ctrl
 * n       = path loss exponent (environment-dependent)
 *
 * The model works because signal power decays logarithmically with distance.
 * At 1m we measure TxPower. Each additional 10*n dB of loss doubles the distance.
 * Example: If n=2.5 and we lose 25dB from TxPower, distance = 10^(25/25) = 10m
 */
#ifdef CONFIG_ESPNOW_TX_POWER_CALIBRATION
#define ESPNOW_TX_POWER_DBM        CONFIG_ESPNOW_TX_POWER_CALIBRATION
#else
#define ESPNOW_TX_POWER_DBM        (-40)
#endif

#ifdef CONFIG_ESPNOW_PATH_LOSS_EXPONENT_X10
#define ESPNOW_PATH_LOSS_EXP       (CONFIG_ESPNOW_PATH_LOSS_EXPONENT_X10 / 10.0f)
#else
#define ESPNOW_PATH_LOSS_EXP       2.5f
#endif

#define RSSI_ZONE_VERY_CLOSE       (-50)
#define RSSI_ZONE_CLOSE            (-60)
#define RSSI_ZONE_MEDIUM           (-70)
#define RSSI_ZONE_FAR              (-80)

/* Event IDs for ESP-NOW callbacks */
typedef enum {
    ESPNOW_SEND_CB,
    ESPNOW_RECV_CB,
} espnow_event_id_t;

/* Send callback event data */
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
} espnow_event_send_cb_t;

/* Receive callback event data */
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t *data;
    int data_len;
    int8_t rssi;
    int8_t noise_floor;
} espnow_event_recv_cb_t;

/* Union of event data types */
typedef union {
    espnow_event_send_cb_t send_cb;
    espnow_event_recv_cb_t recv_cb;
} espnow_event_info_t;

/* Event structure posted to ESP-NOW task */
typedef struct {
    espnow_event_id_t id;
    espnow_event_info_t info;
} espnow_event_t;

/* Data types for ESP-NOW packets */
enum {
    ESPNOW_DATA_BROADCAST,
    ESPNOW_DATA_UNICAST,
    ESPNOW_DATA_MAX,
};

/* ESP-NOW packet data structure */
typedef struct {
    uint8_t type;                         // Broadcast or unicast ESPNOW data.
    uint8_t state;                        // Indicate that if has received broadcast ESPNOW data or not.
    uint16_t seq_num;                     // Sequence number of ESPNOW data.
    uint16_t crc;                         // CRC16 value of ESPNOW data.
    uint32_t magic;                       // Magic number which is used to determine which device to send unicast ESPNOW data.
    uint8_t payload[0];                   // Real payload of ESPNOW data.
} __attribute__((packed)) espnow_data_t;

/* Parameters for sending ESP-NOW data */
typedef struct {
    bool unicast;                         // Send unicast ESPNOW data.
    bool broadcast;                       // Send broadcast ESPNOW data.
    uint8_t state;                        // Indicate that if has received broadcast ESPNOW data or not.
    uint32_t magic;                       // Magic number which is used to determine which device to send unicast ESPNOW data.
    uint16_t delay;                       // Delay between sending two ESPNOW data, unit: ms.
    int len;                              // Length of ESPNOW data to be sent, unit: byte.
    uint8_t *buffer;                      // Buffer pointing to ESPNOW data.
    uint8_t dest_mac[ESP_NOW_ETH_ALEN];   // MAC address of destination device.
} espnow_send_param_t;

/* Broadcast MAC address - exposed for IS_BROADCAST_ADDR macro */
extern const uint8_t espnow_broadcast_mac[ESP_NOW_ETH_ALEN];

/**
 * @brief Initialize ESP-NOW and start the ESP-NOW task
 * 
 * Creates the event queue, registers callbacks, adds broadcast peer,
 * and starts the ESP-NOW task.
 * 
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t espnow_init(void);

#endif /* ESPNOW_H */
