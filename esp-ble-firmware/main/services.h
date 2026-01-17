/*
 * services.h - Shared declarations for BLE services
 *
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#ifndef SERVICES_H
#define SERVICES_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Build Information
 * ============================================================================ */

/** Build date string (set at compile time) */
extern const char *build_date;

/** Build time string (set at compile time) */
extern const char *build_time;

/** Combined build timestamp */
extern const char *build_timestamp;

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * @brief Structure to hold scanned BLE device local name
 */
typedef struct {
    char scan_local_name[32];
    uint8_t name_len;
} ble_scan_local_name_t;

/**
 * @brief Structure for HCI received data packets
 */
typedef struct {
    uint8_t *q_data;
    uint16_t q_data_len;
} host_rcv_data_t;

/* ============================================================================
 * External Variables
 * ============================================================================ */

/** Queue handle for advertising data */
extern QueueHandle_t adv_queue;

/** Counter for scanned advertising reports */
extern uint16_t scanned_count;

/* ============================================================================
 * BLE Task Functions
 * ============================================================================ */

/**
 * @brief Initialize the BLE subsystem
 * 
 * Initializes the Bluetooth controller, registers VHCI callbacks,
 * and creates the advertising queue.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t bt_init(void);

/**
 * @brief Send BLE HCI commands for advertising and scanning
 * 
 * Sends the sequence of HCI commands to configure and start
 * BLE advertising and scanning.
 */
void bt_send_commands(void);

/**
 * @brief HCI event processing task
 * 
 * FreeRTOS task that processes incoming HCI events from the queue.
 * 
 * @param pvParameters Task parameters (unused)
 */
void hci_evt_process(void *pvParameters);

/**
 * @brief Periodic timer callback for logging scan count
 * 
 * @param arg Callback argument (unused)
 */
void periodic_timer_callback(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* SERVICES_H */