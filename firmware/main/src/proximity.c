#include "proximity.h"
#include "buzzer.h"
#include "hnr26_badge.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "proximity";

#define PROXIMITY_TASK_STACK_SIZE   3072
#define PROXIMITY_TASK_PRIORITY     4
#define PROXIMITY_TASK_NAME         "proximity"
#define PROXIMITY_QUEUE_SIZE        10
#define PROXIMITY_LOOP_PERIOD_MS    20
#define PROXIMITY_MAX_LEDS          10

typedef struct {
    uint8_t led_count;
    uint32_t blink_period_ms;
} zone_params_t;

static const zone_params_t ZONE_PARAMS[] = {
    [PROXIMITY_ZONE_UNKNOWN]    = { .led_count = 0,  .blink_period_ms = 0    },
    [PROXIMITY_ZONE_VERY_CLOSE] = { .led_count = 10, .blink_period_ms = 50   },
    [PROXIMITY_ZONE_CLOSE]      = { .led_count = 7,  .blink_period_ms = 100  },
    [PROXIMITY_ZONE_MEDIUM]     = { .led_count = 5,  .blink_period_ms = 200  },
    [PROXIMITY_ZONE_FAR]        = { .led_count = 3,  .blink_period_ms = 400  },
    [PROXIMITY_ZONE_EDGE]       = { .led_count = 1,  .blink_period_ms = 800  },
};

typedef struct {
    int8_t rssi;
} proximity_event_t;

typedef struct {
    bool initialized;
    bool enabled;
    proximity_config_t config;

    TaskHandle_t task_handle;
    QueueHandle_t queue;
    SemaphoreHandle_t mutex;

    int8_t rssi_samples[PROXIMITY_RSSI_SAMPLES];
    uint8_t rssi_index;
    uint8_t rssi_count;
    int16_t rssi_sum;

    proximity_zone_t current_zone;
    int8_t current_rssi;
    TickType_t last_rssi_time;

    bool led_state;
    TickType_t last_toggle_time;
} proximity_state_t;

static proximity_state_t s_state = {0};

static void proximity_task(void *pvParameter);
static proximity_zone_t rssi_to_zone(int8_t rssi);
static void update_rssi_average(int8_t rssi);
static void set_leds(uint8_t count, bool on);
static void all_leds_off(void);

static proximity_zone_t rssi_to_zone(int8_t rssi)
{
    if (rssi >= PROXIMITY_RSSI_VERY_CLOSE) {
        return PROXIMITY_ZONE_VERY_CLOSE;
    } else if (rssi >= PROXIMITY_RSSI_CLOSE) {
        return PROXIMITY_ZONE_CLOSE;
    } else if (rssi >= PROXIMITY_RSSI_MEDIUM) {
        return PROXIMITY_ZONE_MEDIUM;
    } else if (rssi >= PROXIMITY_RSSI_FAR) {
        return PROXIMITY_ZONE_FAR;
    } else {
        return PROXIMITY_ZONE_EDGE;
    }
}

static void update_rssi_average(int8_t rssi)
{
    if (s_state.rssi_count >= PROXIMITY_RSSI_SAMPLES) {
        s_state.rssi_sum -= s_state.rssi_samples[s_state.rssi_index];
    } else {
        s_state.rssi_count++;
    }

    s_state.rssi_samples[s_state.rssi_index] = rssi;
    s_state.rssi_sum += rssi;

    s_state.rssi_index = (s_state.rssi_index + 1) % PROXIMITY_RSSI_SAMPLES;

    s_state.current_rssi = (int8_t)(s_state.rssi_sum / s_state.rssi_count);
}

static void set_leds(uint8_t count, bool on)
{
    aw9523_pin_data_digital_t state = on ? 1 : 0;

    for (uint8_t i = 1; i <= PROXIMITY_MAX_LEDS; i++) {
        if (i <= count) {
            hnr26_badge_set_led(i, state);
        } else {
            hnr26_badge_set_led(i, 0);
        }
    }
}

static void all_leds_off(void)
{
    for (uint8_t i = 1; i <= PROXIMITY_MAX_LEDS; i++) {
        hnr26_badge_set_led(i, 0);
    }
}

static void proximity_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Proximity task started");

    proximity_event_t evt;
    TickType_t now;

    while (1) {
        now = xTaskGetTickCount();
        // TODO: Re-enable when LED logging is fixed
        // hnr26_badge_update_virtual_pins_state();
        if (xQueueReceive(s_state.queue, &evt, pdMS_TO_TICKS(PROXIMITY_LOOP_PERIOD_MS)) == pdTRUE) {
            if (xSemaphoreTake(s_state.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                update_rssi_average(evt.rssi);
                s_state.last_rssi_time = now;
                s_state.current_zone = rssi_to_zone(s_state.current_rssi);
                xSemaphoreGive(s_state.mutex);

                ESP_LOGD(TAG, "RSSI: %d dBm (avg: %d), zone: %d",
                         evt.rssi, s_state.current_rssi, s_state.current_zone);
            }
        }

        if (!s_state.enabled) {
            // TODO: Re-enable when LED logging is fixed
            // all_leds_off();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if ((now - s_state.last_rssi_time) > pdMS_TO_TICKS(PROXIMITY_TIMEOUT_MS)) {
            if (s_state.current_zone != PROXIMITY_ZONE_UNKNOWN) {
                ESP_LOGD(TAG, "RSSI timeout, entering UNKNOWN zone");
                s_state.current_zone = PROXIMITY_ZONE_UNKNOWN;
                // TODO: Re-enable when LED logging is fixed
                // all_leds_off();
                buzzer_stop();
            }
            continue;
        }

        const zone_params_t *params = &ZONE_PARAMS[s_state.current_zone];

        if (params->led_count == 0 || params->blink_period_ms == 0) {
            continue;
        }

        TickType_t toggle_period = pdMS_TO_TICKS(params->blink_period_ms);
        if ((now - s_state.last_toggle_time) >= toggle_period) {
            s_state.led_state = !s_state.led_state;
            s_state.last_toggle_time = now;

            // TODO: Re-enable when LED logging is fixed
            // if (s_state.config.enable_leds) {
            //     set_leds(params->led_count, s_state.led_state);
            // }

            if (s_state.led_state && s_state.config.enable_buzzer) {
                buzzer_beep(params->blink_period_ms / 2, 0, 1);
            }
        }
    }
}

esp_err_t proximity_init(const proximity_config_t *config)
{
    if (s_state.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_state, 0, sizeof(s_state));

    if (config) {
        memcpy(&s_state.config, config, sizeof(proximity_config_t));
    } else {
        s_state.config = (proximity_config_t)PROXIMITY_CONFIG_DEFAULT();
    }

    s_state.mutex = xSemaphoreCreateMutex();
    if (s_state.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_state.queue = xQueueCreate(PROXIMITY_QUEUE_SIZE, sizeof(proximity_event_t));
    if (s_state.queue == NULL) {
        ESP_LOGE(TAG, "Failed to create queue");
        vSemaphoreDelete(s_state.mutex);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(
        proximity_task,
        PROXIMITY_TASK_NAME,
        PROXIMITY_TASK_STACK_SIZE,
        NULL,
        PROXIMITY_TASK_PRIORITY,
        &s_state.task_handle
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        vQueueDelete(s_state.queue);
        vSemaphoreDelete(s_state.mutex);
        return ESP_ERR_NO_MEM;
    }

    s_state.initialized = false;
    s_state.enabled = false;
    s_state.current_zone = PROXIMITY_ZONE_UNKNOWN;
    s_state.last_rssi_time = xTaskGetTickCount();
    s_state.last_toggle_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "Initialized (buzzer: %s, LEDs: %s, volume: %d%%)",
             s_state.config.enable_buzzer ? "on" : "off",
             s_state.config.enable_leds ? "on" : "off",
             s_state.config.buzzer_volume);

    return ESP_OK;
}

void proximity_update(int8_t rssi)
{
    if (!s_state.initialized || s_state.queue == NULL) {
        return;
    }

    proximity_event_t evt = { .rssi = rssi };

    xQueueSend(s_state.queue, &evt, 0);
}

proximity_zone_t proximity_get_zone(void)
{
    return s_state.current_zone;
}

int8_t proximity_get_rssi(void)
{
    return s_state.current_rssi;
}

void proximity_enable(bool enable)
{
    if (xSemaphoreTake(s_state.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_state.enabled = enable;
        if (!enable) {
            // TODO: Re-enable when LED logging is fixed
            // all_leds_off();
            buzzer_stop();
        }
        xSemaphoreGive(s_state.mutex);
        ESP_LOGI(TAG, "Proximity alerts %s", enable ? "enabled" : "disabled");
    }
}

bool proximity_is_enabled(void)
{
    return s_state.enabled;
}

esp_err_t proximity_deinit(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    proximity_enable(false);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_state.task_handle) {
        vTaskDelete(s_state.task_handle);
        s_state.task_handle = NULL;
    }

    if (s_state.queue) {
        vQueueDelete(s_state.queue);
        s_state.queue = NULL;
    }

    if (s_state.mutex) {
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
    }

    s_state.initialized = false;
    ESP_LOGI(TAG, "Deinitialized");

    return ESP_OK;
}
