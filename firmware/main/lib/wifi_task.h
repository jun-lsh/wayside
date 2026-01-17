#ifndef WIFI_TASK_H
#define WIFI_TASK_H

#include "esp_err.h"

/**
 * @brief Initialize WiFi for ESP-NOW operation
 * 
 * Initializes the network interface, event loop, and WiFi in the configured mode.
 * Must be called before espnow_init().
 */
void wifi_init(void);

#endif /* WIFI_TASK_H */
