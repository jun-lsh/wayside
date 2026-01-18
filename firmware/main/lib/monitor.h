#ifndef MONITOR_H
#define MONITOR_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// monitor data structure
typedef struct {
    int voltage_mv;      // adc voltage in millivolts
    float temperature_c; // internal temp in celsius
    uint32_t timestamp;  // tick count when sampled
} monitor_data_t;

// init monitor task (reads voltage and temp every 5 seconds)
// adc_channel: channel to read voltage from (e.g. ADC_CHANNEL_0)
// returns queue handle for receiving data (queue size 1)
esp_err_t monitor_init(int adc_channel, QueueHandle_t *out_queue);

// get latest data without blocking (returns false if no data)
bool monitor_get_latest(monitor_data_t *data);

// stop monitor task
void monitor_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
