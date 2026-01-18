/**
 * @file button_task.h
 * @brief Button monitoring task with long-press detection
 * 
 * Monitors buttons on the AW9523 GPIO expander and detects long press
 * events. Uses a queue to notify other modules (e.g., buzzer mute toggle).
 */

#ifndef BUTTON_TASK_H
#define BUTTON_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "aw9523.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default button pin (P1_4 = pin 12) */
#define BUTTON_TASK_DEFAULT_PIN     12

/** Default long press duration in milliseconds */
#define BUTTON_TASK_LONG_PRESS_MS   1000

/** Default polling interval in milliseconds */
#define BUTTON_TASK_POLL_MS         20

/**
 * @brief Button task configuration
 */
typedef struct {
    aw9523_t *gpio_expander;        /**< Pointer to GPIO expander device handle */
    aw9523_pin_num_t button_pin;    /**< Button pin number (0-15) */
    uint32_t long_press_ms;         /**< Long press threshold in ms */
    uint32_t poll_interval_ms;      /**< Polling interval in ms */
    QueueHandle_t notify_queue;     /**< Queue to send toggle notifications (length 1) */
} button_task_config_t;

/**
 * @brief Default configuration macro
 */
#define BUTTON_TASK_CONFIG_DEFAULT() { \
    .gpio_expander = NULL, \
    .button_pin = BUTTON_TASK_DEFAULT_PIN, \
    .long_press_ms = BUTTON_TASK_LONG_PRESS_MS, \
    .poll_interval_ms = BUTTON_TASK_POLL_MS, \
    .notify_queue = NULL \
}

/**
 * @brief Initialize and start the button monitoring task
 * 
 * @param config Pointer to configuration structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t button_task_init(const button_task_config_t *config);

/**
 * @brief Stop and deinitialize the button task
 * 
 * @return ESP_OK on success
 */
esp_err_t button_task_deinit(void);

/**
 * @brief Check if button task is running
 * 
 * @return true if running, false otherwise
 */
bool button_task_is_running(void);

/**
 * @brief Get the number of long presses detected
 * 
 * @return Count of long press events
 */
uint32_t button_task_get_press_count(void);

#ifdef __cplusplus
}
#endif

#endif /* BUTTON_TASK_H */
