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
 * @brief Initialize BLE 5.0 UART service with extended advertising
 * 
 * This function initializes the Bluedroid stack, configures the GATT server
 * with Nordic UART Service (NUS) compatible UUIDs, and starts extended
 * advertising (BLE 5.0).
 * 
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t ble_init(void);

// Start advertising for a specific duration in seconds (0 = forever)
// Call this when NFC event occurs
esp_err_t ble_start_pairing(uint32_t timeout_sec);

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

/**
 * @brief Get BLE MAC address (valid after ble_init)
 * 
 * @param mac Buffer to store 6-byte MAC address
 * @return ESP_OK on success
 */
esp_err_t ble_get_mac(uint8_t *mac);

/**
 * @brief Get device name (valid after ble_init)
 * 
 * @return Pointer to device name string
 */
const char *ble_get_device_name(void);

/**
 * @brief Set PHY mode for advertising (BLE 5.0)
 * 
 * @param primary_phy Primary PHY (ESP_BLE_GAP_PHY_1M, ESP_BLE_GAP_PHY_CODED)
 * @param secondary_phy Secondary PHY (ESP_BLE_GAP_PHY_1M, ESP_BLE_GAP_PHY_2M, ESP_BLE_GAP_PHY_CODED)
 * @return ESP_OK on success
 */
esp_err_t ble_set_adv_phy(esp_ble_gap_phy_t primary_phy, esp_ble_gap_phy_t secondary_phy);

/**
 * @brief Enable or disable long range mode (coded PHY)
 * 
 * Long range mode uses coded PHY which provides ~4x range at lower data rate.
 * Note: Not all phones support coded PHY scanning.
 * 
 * @param enable true to enable coded PHY, false for 1M PHY
 * @return ESP_OK on success
 */
esp_err_t ble_enable_long_range(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* BLE_TASK_H */
