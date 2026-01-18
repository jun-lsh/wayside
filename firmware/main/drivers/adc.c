#include "adc.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "adc";

// create calibration scheme (curve fitting for esp32c3, uses efuse data)
static esp_err_t create_calibration(adc_ctx_t *ctx, adc_unit_t unit, adc_atten_t atten)
{
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &ctx->cali_handle);
    if (err == ESP_OK) {
        ctx->calibrated = true;
        ESP_LOGI(TAG, "calibration scheme created");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "calibration not supported, using raw values");
        ctx->calibrated = false;
        err = ESP_OK; // not fatal
    }
    return err;
}

esp_err_t adc_init(adc_ctx_t *ctx, adc_unit_t unit)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    
    memset(ctx, 0, sizeof(adc_ctx_t));
    
    // init adc oneshot
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &ctx->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc unit init failed: %s", esp_err_to_name(err));
        return err;
    }
    
    // create calibration (uses efuse data on esp32c3)
    err = create_calibration(ctx, unit, ADC_ATTEN_DB_12);
    if (err != ESP_OK) {
        adc_oneshot_del_unit(ctx->handle);
        return err;
    }
    
    return ESP_OK;
}

esp_err_t adc_config_channel(adc_ctx_t *ctx, adc_channel_t channel, adc_atten_t atten)
{
    if (!ctx || !ctx->handle) return ESP_ERR_INVALID_STATE;
    
    adc_oneshot_chan_cfg_t cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = atten,
    };
    
    return adc_oneshot_config_channel(ctx->handle, channel, &cfg);
}

esp_err_t adc_read_raw(adc_ctx_t *ctx, adc_channel_t channel, int *raw)
{
    if (!ctx || !ctx->handle || !raw) return ESP_ERR_INVALID_ARG;
    return adc_oneshot_read(ctx->handle, channel, raw);
}

esp_err_t adc_read_voltage(adc_ctx_t *ctx, adc_channel_t channel, int *voltage_mv)
{
    if (!ctx || !ctx->handle || !voltage_mv) return ESP_ERR_INVALID_ARG;
    
    int raw;
    esp_err_t err = adc_oneshot_read(ctx->handle, channel, &raw);
    if (err != ESP_OK) return err;
    
    if (ctx->calibrated && ctx->cali_handle) {
        return adc_cali_raw_to_voltage(ctx->cali_handle, raw, voltage_mv);
    }
    
    // fallback: rough conversion without calibration (assuming 12-bit, 3.3v ref)
    *voltage_mv = (raw * 3300) / 4095;
    return ESP_OK;
}

esp_err_t adc_deinit(adc_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    
    if (ctx->cali_handle) {
        adc_cali_delete_scheme_curve_fitting(ctx->cali_handle);
        ctx->cali_handle = NULL;
    }
    
    if (ctx->handle) {
        adc_oneshot_del_unit(ctx->handle);
        ctx->handle = NULL;
    }
    
    ctx->calibrated = false;
    return ESP_OK;
}

esp_err_t temp_sensor_init(temp_sensor_ctx_t *ctx, int range_min, int range_max)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    
    memset(ctx, 0, sizeof(temp_sensor_ctx_t));
    
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(range_min, range_max);
    esp_err_t err = temperature_sensor_install(&cfg, &ctx->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "temp sensor install failed: %s", esp_err_to_name(err));
        return err;
    }
    
    err = temperature_sensor_enable(ctx->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "temp sensor enable failed: %s", esp_err_to_name(err));
        temperature_sensor_uninstall(ctx->handle);
        ctx->handle = NULL;
        return err;
    }
    
    ctx->enabled = true;
    ESP_LOGI(TAG, "temp sensor initialized (range %d-%d C)", range_min, range_max);
    return ESP_OK;
}

esp_err_t temp_sensor_read(temp_sensor_ctx_t *ctx, float *celsius)
{
    if (!ctx || !ctx->handle || !celsius) return ESP_ERR_INVALID_ARG;
    if (!ctx->enabled) return ESP_ERR_INVALID_STATE;
    
    return temperature_sensor_get_celsius(ctx->handle, celsius);
}

esp_err_t temp_sensor_deinit(temp_sensor_ctx_t *ctx)
{
    if (!ctx) return ESP_ERR_INVALID_ARG;
    
    if (ctx->handle) {
        if (ctx->enabled) {
            temperature_sensor_disable(ctx->handle);
        }
        temperature_sensor_uninstall(ctx->handle);
        ctx->handle = NULL;
    }
    
    ctx->enabled = false;
    return ESP_OK;
}