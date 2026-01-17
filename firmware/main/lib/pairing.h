#ifndef PAIRING_H
#define PAIRING_H

#include "esp_err.h"
#include "esp_now.h"
#include "states.h"

#define PAIRING_TIMEOUT_MS      2000
#define PAIRING_REBROADCAST_MS  250
#define PAIRING_HEARTBEAT_MS    1000
#define PAIRING_HEARTBEAT_MISS_MAX  5

/**
 * Pairing context - maintains state machine state
 */
typedef struct {
    BROADCAST_STATE current_state;              /* Current pairing state */
    uint8_t partner_mac[ESP_NOW_ETH_ALEN];      /* MAC of paired/proposed partner */
    uint8_t my_mac[ESP_NOW_ETH_ALEN];           /* This device's MAC address */
    uint32_t last_action_time;                  /* Timestamp of last action (ms) */
    uint8_t bitmask;                            /* Application-specific flags */
    
    /* Heartbeat tracking */
    uint32_t last_heartbeat_sent;               /* When we last sent a heartbeat */
    uint32_t last_heartbeat_recv;               /* When we last received a heartbeat */
    uint16_t heartbeat_seq;                     /* Outgoing heartbeat sequence number */
    uint16_t partner_seq;                       /* Last received partner sequence */
    int8_t partner_rssi;                        /* Last RSSI from partner */
    uint8_t missed_heartbeats;                  /* Count of missed heartbeats */
} pairing_ctx_t;

/**
 * @brief Initialize the pairing context
 * 
 * Sets up the context with the device's MAC address and initial SEARCHING state.
 * Must be called before any other pairing functions.
 * 
 * @param ctx Pointer to pairing context to initialize
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if ctx is NULL
 */
esp_err_t pairing_init(pairing_ctx_t *ctx);

/**
 * @brief Handle received ESP-NOW data for pairing protocol
 * 
 * Core callback handler that processes incoming pairing messages and
 * performs state transitions. Should be called from the ESP-NOW receive callback.
 * 
 * @param ctx Pointer to pairing context
 * @param mac_addr MAC address of the sender
 * @param data Pointer to received data (will be cast to broadcast_t)
 * @param len Length of received data
 * @param rssi Signal strength (for potential proximity-based decisions)
 */
void pairing_handle_recv(pairing_ctx_t *ctx, const uint8_t *mac_addr, 
                         const uint8_t *data, int len, int8_t rssi);

/**
 * @brief Periodic tick function for pairing maintenance
 * 
 * Handles timeouts and periodic rebroadcasting. Should be called
 * regularly from the main loop or a timer.
 * 
 * @param ctx Pointer to pairing context
 */
void pairing_tick(pairing_ctx_t *ctx);

/**
 * @brief Get the current pairing state
 * 
 * @param ctx Pointer to pairing context
 * @return Current BROADCAST_STATE
 */
BROADCAST_STATE pairing_get_state(const pairing_ctx_t *ctx);

/**
 * @brief Check if device is currently paired
 * 
 * @param ctx Pointer to pairing context
 * @return true if paired, false otherwise
 */
bool pairing_is_paired(const pairing_ctx_t *ctx);

/**
 * @brief Get partner MAC address (only valid when paired)
 * 
 * @param ctx Pointer to pairing context
 * @param out_mac Buffer to copy partner MAC into (must be ESP_NOW_ETH_ALEN bytes)
 * @return ESP_OK if paired and MAC copied, ESP_ERR_INVALID_STATE if not paired
 */
esp_err_t pairing_get_partner_mac(const pairing_ctx_t *ctx, uint8_t *out_mac);

/**
 * @brief Reset pairing state back to SEARCHING
 * 
 * Useful for manually breaking a pairing or recovering from errors.
 * 
 * @param ctx Pointer to pairing context
 */
void pairing_reset(pairing_ctx_t *ctx);

#endif /* PAIRING_H */
