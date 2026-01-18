/**
 * @file buzzer_fuet7525.h
 * @brief Driver for FUET-7525-3.6V Magnetic Buzzer on ESP32
 * 
 * Datasheet specs:
 *   - Oscillation Frequency: 2700 Hz
 *   - Operating Voltage: 2.5-4.5V (Vo-p)
 *   - Rated Voltage: 3.6V (Vo-p)
 *   - Drive: 50% duty cycle square wave
 *   - Coil Resistance: 16±3Ω
 *   - Max Current: 100mA
 * 
 * Volume control is achieved via PWM duty cycle modulation.
 * The buzzer requires a transistor driver circuit (see datasheet section 5).
 */

#ifndef BUZZER_FUET7525_H
#define BUZZER_FUET7525_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default GPIO pin for buzzer output */
#define BUZZER_DEFAULT_GPIO     3

/** Buzzer resonant frequency from datasheet (Hz) */
#define BUZZER_FREQ_HZ          2700

/** Volume levels (0-100%) */
#define BUZZER_VOLUME_MIN       0
#define BUZZER_VOLUME_MAX       100
#define BUZZER_VOLUME_DEFAULT   50

/** Volume step for increase/decrease functions */
#define BUZZER_VOLUME_STEP      10

/**
 * @brief Buzzer configuration structure
 */
typedef struct {
    int gpio_num;           /**< GPIO pin number for buzzer output */
    uint32_t frequency;     /**< Oscillation frequency in Hz (default 2700) */
    uint8_t initial_volume; /**< Initial volume 0-100 (maps to duty cycle) */
} buzzer_config_t;

/**
 * @brief Default configuration macro
 */
#define BUZZER_CONFIG_DEFAULT() { \
    .gpio_num = BUZZER_DEFAULT_GPIO, \
    .frequency = BUZZER_FREQ_HZ, \
    .initial_volume = BUZZER_VOLUME_DEFAULT \
}

/**
 * @brief Initialize the buzzer driver
 * 
 * Sets up LEDC peripheral for PWM generation and creates the buzzer task.
 * 
 * @param config Pointer to configuration structure, or NULL for defaults
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t buzzer_init(const buzzer_config_t *config);

/**
 * @brief Deinitialize the buzzer driver
 * 
 * Stops the buzzer, deletes the task, and releases resources.
 * 
 * @return ESP_OK on success
 */
esp_err_t buzzer_deinit(void);

/**
 * @brief Start the buzzer (continuous tone)
 * 
 * @return ESP_OK on success
 */
esp_err_t buzzer_start(void);

/**
 * @brief Stop the buzzer
 * 
 * @return ESP_OK on success
 */
esp_err_t buzzer_stop(void);

/**
 * @brief Check if buzzer is currently playing
 * 
 * @return true if buzzer is active, false otherwise
 */
bool buzzer_is_playing(void);

/**
 * @brief Set buzzer volume
 * 
 * Volume is controlled by PWM duty cycle. Higher duty cycle = louder.
 * Note: At 50% duty cycle, the buzzer operates at its rated spec.
 * 
 * @param volume Volume level 0-100
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t buzzer_set_volume(uint8_t volume);

/**
 * @brief Get current volume level
 * 
 * @return Current volume 0-100
 */
uint8_t buzzer_get_volume(void);

/**
 * @brief Increase volume by one step
 * 
 * @return ESP_OK on success, new volume level via buzzer_get_volume()
 */
esp_err_t buzzer_volume_up(void);

/**
 * @brief Decrease volume by one step
 * 
 * @return ESP_OK on success, new volume level via buzzer_get_volume()
 */
esp_err_t buzzer_volume_down(void);

/**
 * @brief Set buzzer frequency
 * 
 * Allows deviation from the 2700Hz resonant frequency for different tones.
 * Note: Best sound output is at the resonant frequency (2700Hz).
 * 
 * @param freq_hz Frequency in Hz
 * @return ESP_OK on success
 */
esp_err_t buzzer_set_frequency(uint32_t freq_hz);

/**
 * @brief Get current frequency
 * 
 * @return Current frequency in Hz
 */
uint32_t buzzer_get_frequency(void);

/**
 * @brief Play a beep pattern
 * 
 * Non-blocking - pattern plays in background task.
 * 
 * @param on_ms  Duration of tone in milliseconds
 * @param off_ms Duration of silence in milliseconds
 * @param count  Number of beeps (0 = infinite until buzzer_stop())
 * @return ESP_OK on success
 */
esp_err_t buzzer_beep(uint32_t on_ms, uint32_t off_ms, uint32_t count);

/**
 * @brief Play a single short beep
 * 
 * Convenience function for a quick 100ms beep.
 * 
 * @return ESP_OK on success
 */
esp_err_t buzzer_beep_once(void);

/**
 * @brief Play a melody/sequence of tones
 * 
 * @param frequencies Array of frequencies (Hz), 0 = rest
 * @param durations   Array of durations (ms) for each tone
 * @param length      Number of notes in the sequence
 * @return ESP_OK on success
 */
esp_err_t buzzer_play_sequence(const uint32_t *frequencies, 
                                const uint32_t *durations, 
                                size_t length);

/**
 * @brief Toggle buzzer mute state
 * 
 * Sends a toggle message to the buzzer task's queue.
 * When muted, all sound commands are silently ignored.
 * 
 * @return ESP_OK on success
 */
esp_err_t buzzer_toggle_mute(void);

/**
 * @brief Set buzzer mute state directly
 * 
 * @param muted true to mute, false to unmute
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if mutex timeout
 */
esp_err_t buzzer_set_muted(bool muted);

/**
 * @brief Check if buzzer is muted
 * 
 * @return true if muted, false otherwise
 */
bool buzzer_is_muted(void);

/**
 * @brief Get the toggle queue handle
 * 
 * External tasks can use this queue to trigger mute toggle.
 * Queue length is 1, use xQueueOverwrite() to send.
 * 
 * @return QueueHandle_t for the toggle queue
 */
QueueHandle_t buzzer_get_toggle_queue(void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_FUET7525_H */
