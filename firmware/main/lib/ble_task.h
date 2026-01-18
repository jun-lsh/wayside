/*
 * ble_task.h - BLE UART service with passkey authentication
 */

#ifndef BLE_TASK_H
#define BLE_TASK_H

#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include <stdbool.h>
#include <stdint.h>

#define BLE_MESSAGE_DELIMITER_CHAR '\r'
#define BLE_MESSAGE_DELIMITER_STR "\r"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize BLE UART service
 * 
 * Initializes Bluedroid stack and GATT server with Nordic UART Service.
 * Does NOT start advertising - call ble_start_pairing_with_passkey() for that.
 * 
 * @return ESP_OK on success
 */
esp_err_t ble_init(void);

/**
 * @brief Start BLE advertising with passkey authentication
 * 
 * Configures BLE security to require passkey entry and starts advertising.
 * The passkey should come from NFC tag (written by nfc_pair module).
 * 
 * @param passkey 6-digit passkey (100000-999999)
 * @param timeout_sec Advertising timeout in seconds (0 = no timeout)
 * @return ESP_OK on success
 */
esp_err_t ble_start_pairing_with_passkey(uint32_t passkey, uint32_t timeout_sec);

/**
 * @brief Start BLE advertising without passkey (Just Works)
 * 
 * For backwards compatibility or when security is not required.
 * 
 * @param timeout_sec Advertising timeout in seconds (0 = no timeout)
 * @return ESP_OK on success
 */
esp_err_t ble_start_pairing(uint32_t timeout_sec);

/**
 * @brief Stop BLE advertising
 */
void ble_stop_advertising(void);

/**
 * @brief Send a message to the connected BLE device
 * 
 * @param message Null-terminated string to send
 */
void ble_send_message(const char *message);

/**
 * @brief Check if a BLE device is connected
 */
bool ble_is_connected(void);

/**
 * @brief Check if device is paired (authenticated)
 */
bool ble_is_paired(void);

/**
 * @brief Get BLE MAC address
 * 
 * @param mac Buffer for 6-byte MAC address
 * @return ESP_OK on success
 */
esp_err_t ble_get_mac(uint8_t *mac);

/**
 * @brief Get device name
 */
const char *ble_get_device_name(void);

/**
 * @brief Set PHY mode for advertising (BLE 5.0)
 */
esp_err_t ble_set_adv_phy(esp_ble_gap_phy_t primary_phy, esp_ble_gap_phy_t secondary_phy);

/**
 * @brief Enable/disable long range mode (coded PHY)
 */
esp_err_t ble_enable_long_range(bool enable);

/**
 * @brief Disconnect current client
 */
esp_err_t ble_disconnect(void);

/**
 * @brief Callback type for connection events
 */
typedef void (*ble_connection_cb_t)(bool connected, void *arg);

/**
 * @brief Callback type for authentication events  
 */
typedef void (*ble_auth_cb_t)(bool success, void *arg);

/**
 * @brief Set connection callback
 */
void ble_set_connection_callback(ble_connection_cb_t cb, void *arg);

/**
 * @brief Set authentication callback
 */
void ble_set_auth_callback(ble_auth_cb_t cb, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* BLE_TASK_H */
