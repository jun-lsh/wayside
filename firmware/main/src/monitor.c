#include "monitor.h"
#include "adc.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "monitor";

#define MONITOR_INTERVAL_MS    5000
#define MONITOR_STACK_SIZE     4096
#define MONITOR_PRIORITY       3

static adc_ctx_t s_adc_ctx;
static temp_sensor_ctx_t s_temp_ctx;
static QueueHandle_t s_data_queue = NULL;
static TaskHandle_t s_task_handle = NULL;
static int s_adc_channel = 0;
static monitor_data_t s_latest_data;
static bool s_running = false;

// monitor task
static void monitor_task(void *arg)
{
    monitor_data_t data;
    
    while (s_running) {
        // read voltage
        int voltage = 0;
        esp_err_t err = adc_read_voltage(&s_adc_ctx, s_adc_channel, &voltage);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc read failed: %s", esp_err_to_name(err));
            voltage = -1;
        }
        data.voltage_mv = voltage;
        
        // read temperature
        float temp = 0;
        err = temp_sensor_read(&s_temp_ctx, &temp);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "temp read failed: %s", esp_err_to_name(err));
            temp = -999.0f;
        }
        data.temperature_c = temp;
        data.timestamp = xTaskGetTickCount();
        
        // log the values
        ESP_LOGI(TAG, "voltage: %dmV, temp: %.1fC", data.voltage_mv, data.temperature_c);
        
        // update queue (overwrite if full since size is 1)
        xQueueOverwrite(s_data_queue, &data);
        
        // update latest cache
        s_latest_data = data;
        
        vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS));
    }
    
    vTaskDelete(NULL);
}

esp_err_t monitor_init(int adc_channel, QueueHandle_t *out_queue)
{
    esp_err_t ret;
    
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_adc_channel = adc_channel;
    memset(&s_latest_data, 0, sizeof(s_latest_data));
    
    // init adc
    ret = adc_init(&s_adc_ctx, ADC_UNIT_1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // configure channel with 12db attenuation for full range
    ret = adc_config_channel(&s_adc_ctx, adc_channel, ADC_ATTEN_DB_12);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adc channel config failed: %s", esp_err_to_name(ret));
        adc_deinit(&s_adc_ctx);
        return ret;
    }
    
    // init temp sensor (range 10-80c covers typical operating temps)
    ret = temp_sensor_init(&s_temp_ctx, 10, 80);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "temp sensor init failed: %s", esp_err_to_name(ret));
        adc_deinit(&s_adc_ctx);
        return ret;
    }
    
    // create queue with size 1 (latest value only)
    s_data_queue = xQueueCreate(1, sizeof(monitor_data_t));
    if (s_data_queue == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        temp_sensor_deinit(&s_temp_ctx);
        adc_deinit(&s_adc_ctx);
        return ESP_ERR_NO_MEM;
    }
    
    if (out_queue) {
        *out_queue = s_data_queue;
    }
    
    // start task
    s_running = true;
    BaseType_t xret = xTaskCreate(monitor_task, "monitor", MONITOR_STACK_SIZE, 
                                   NULL, MONITOR_PRIORITY, &s_task_handle);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_running = false;
        vQueueDelete(s_data_queue);
        s_data_queue = NULL;
        temp_sensor_deinit(&s_temp_ctx);
        adc_deinit(&s_adc_ctx);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "monitor started (adc ch%d, interval %dms)", adc_channel, MONITOR_INTERVAL_MS);
    return ESP_OK;
}

bool monitor_get_latest(monitor_data_t *data)
{
    if (!data || !s_running) {
        return false;
    }
    
    // try to peek from queue without removing
    if (xQueuePeek(s_data_queue, data, 0) == pdTRUE) {
        return true;
    }
    
    return false;
}

void monitor_deinit(void)
{
    if (!s_running) {
        return;
    }
    
    s_running = false;
    
    // wait for task to exit
    vTaskDelay(pdMS_TO_TICKS(100));
    
    if (s_data_queue) {
        vQueueDelete(s_data_queue);
        s_data_queue = NULL;
    }
    
    temp_sensor_deinit(&s_temp_ctx);
    adc_deinit(&s_adc_ctx);
    
    ESP_LOGI(TAG, "monitor stopped");
}
