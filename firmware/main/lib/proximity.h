/**
 * @file proximity.h
 * @brief Proximity alert module - RSSI-based LED and buzzer feedback
 *
 * This module monitors RSSI values from ESP-NOW packets and provides
 * visual (LEDs) and auditory (buzzer) feedback based on proximity zones.
 * As devices get closer, more LEDs light up and blink/beep faster.
 */

#ifndef PROXIMITY_H
#define PROXIMITY_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Proximity zones based on RSSI thresholds
 *
 * Zones are mapped to different LED counts and blink/beep frequencies.
 * VERY_CLOSE = closest (strongest signal), EDGE = farthest (weakest signal).
 */
typedef enum {
    PROXIMITY_ZONE_UNKNOWN = 0,     /**< No signal or timeout */
    PROXIMITY_ZONE_VERY_CLOSE,      /**< >= -50 dBm: 10 LEDs, 50ms blink */
    PROXIMITY_ZONE_CLOSE,           /**< >= -60 dBm: 7 LEDs, 100ms blink */
    PROXIMITY_ZONE_MEDIUM,          /**< >= -70 dBm: 5 LEDs, 200ms blink */
    PROXIMITY_ZONE_FAR,             /**< >= -80 dBm: 3 LEDs, 400ms blink */
    PROXIMITY_ZONE_EDGE             /**< < -80 dBm: 1 LED, 800ms blink */
} proximity_zone_t;

/**
 * @brief RSSI thresholds for zone classification (in dBm)
 */
#define PROXIMITY_RSSI_VERY_CLOSE   (-50)
#define PROXIMITY_RSSI_CLOSE        (-60)
#define PROXIMITY_RSSI_MEDIUM       (-70)
#define PROXIMITY_RSSI_FAR          (-80)

/**
 * @brief Timeout in milliseconds before transitioning to UNKNOWN zone
 */
#define PROXIMITY_TIMEOUT_MS        1000

/**
 * @brief Number of RSSI samples for moving average filter
 */
#define PROXIMITY_RSSI_SAMPLES      5

/**
 * @brief Configuration for proximity alert behavior
 */
typedef struct {
    bool enable_buzzer;     /**< Enable buzzer feedback */
    bool enable_leds;       /**< Enable LED feedback */
    uint8_t buzzer_volume;  /**< Buzzer volume 0-100 (constant) */
} proximity_config_t;

/**
 * @brief Default configuration macro
 */
#define PROXIMITY_CONFIG_DEFAULT() { \
    .enable_buzzer = true, \
    .enable_leds = true, \
    .buzzer_volume = 100 \
}

/**
 * @brief Initialize the proximity alert module
 *
 * Creates the proximity task and initializes internal state.
 * Requires buzzer_init() and hnr26_badge_init() to be called first.
 *
 * @param config Pointer to configuration, or NULL for defaults
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t proximity_init(const proximity_config_t *config);

/**
 * @brief Update proximity with a new RSSI reading
 *
 * Call this function whenever an ESP-NOW packet is received.
 * The RSSI value is added to the moving average filter.
 * Thread-safe: can be called from any task.
 *
 * @param rssi RSSI value in dBm (typically -100 to 0)
 */
void proximity_update(int8_t rssi);

/**
 * @brief Get the current proximity zone
 *
 * @return Current proximity zone based on averaged RSSI
 */
proximity_zone_t proximity_get_zone(void);

/**
 * @brief Get the current smoothed RSSI value
 *
 * @return Averaged RSSI in dBm, or 0 if no samples
 */
int8_t proximity_get_rssi(void);

/**
 * @brief Enable or disable proximity alerts
 *
 * When disabled, LEDs are turned off and buzzer stops.
 *
 * @param enable true to enable, false to disable
 */
void proximity_enable(bool enable);

/**
 * @brief Check if proximity alerts are enabled
 *
 * @return true if enabled, false if disabled
 */
bool proximity_is_enabled(void);

/**
 * @brief Deinitialize the proximity module
 *
 * Stops the task and releases resources.
 *
 * @return ESP_OK on success
 */
esp_err_t proximity_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* PROXIMITY_H */
