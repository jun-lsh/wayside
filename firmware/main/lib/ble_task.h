#ifndef BLE_TASK_H
#define BLE_TASK_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize BLE UART service and start the BLE task
 * 
 * This function initializes the Bluedroid stack, configures the GATT server
 * with Nordic UART Service (NUS) compatible UUIDs, and starts advertising.
 * A FreeRTOS task is created to handle BLE events.
 * 
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t ble_init(void);

/**
 * @brief Send a message to the connected BLE device via notification
 * 
 * @param message Null-terminated string to send (delimiter is added automatically if needed)
 */
void ble_send_message(const char *message);

/**
 * @brief Check if a BLE device is currently connected
 * 
 * @return true if connected, false otherwise
 */
bool ble_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_TASK_H */
