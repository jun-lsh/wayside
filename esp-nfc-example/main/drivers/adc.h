#ifndef ADC_H
#define ADC_H

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "driver/temperature_sensor.h"

typedef struct {
    adc_oneshot_unit_handle_t handle;
    adc_cali_handle_t cali_handle;
    bool calibrated;
} adc_ctx_t;

typedef struct {
    temperature_sensor_handle_t handle;
    bool enabled;
} temp_sensor_ctx_t;

// init adc for specified unit (ADC_UNIT_1 or ADC_UNIT_2)
// note: adc2 is unreliable on esp32c3
esp_err_t adc_init(adc_ctx_t *ctx, adc_unit_t unit);

// configure a channel with specified attenuation
esp_err_t adc_config_channel(adc_ctx_t *ctx, adc_channel_t channel, adc_atten_t atten);

// read raw adc value
esp_err_t adc_read_raw(adc_ctx_t *ctx, adc_channel_t channel, int *raw);

// read calibrated voltage in mv (returns raw if not calibrated)
esp_err_t adc_read_voltage(adc_ctx_t *ctx, adc_channel_t channel, int *voltage_mv);

// cleanup
esp_err_t adc_deinit(adc_ctx_t *ctx);

// init internal temp sensor with expected range (e.g. 20, 50 for room temp)
esp_err_t temp_sensor_init(temp_sensor_ctx_t *ctx, int range_min, int range_max);

// read temperature in celsius
esp_err_t temp_sensor_read(temp_sensor_ctx_t *ctx, float *celsius);

// cleanup
esp_err_t temp_sensor_deinit(temp_sensor_ctx_t *ctx);

#endif