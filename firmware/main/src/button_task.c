/**
 * @file button_task.c
 * @brief Button monitoring task with long-press detection
 * 
 * Polls a button on the AW9523 GPIO expander and detects long press events.
 * When a long press is detected, sends a notification to the configured queue.
 */

#include "button_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "button_task";

#define BUTTON_TASK_STACK_SIZE  2048
#define BUTTON_TASK_PRIORITY    4
#define BUTTON_TASK_NAME        "button_task"

typedef enum {
    BTN_STATE_IDLE,         /* Button not pressed */
    BTN_STATE_PRESSED,      /* Button pressed, timing */
    BTN_STATE_LONG_FIRED,   /* Long press fired, waiting for release */
} button_state_t;

typedef struct {
    bool initialized;
    bool running;
    TaskHandle_t task_handle;
    
    /* Configuration (copied) */
    aw9523_t *gpio_expander;
    aw9523_pin_num_t button_pin;
    uint32_t long_press_ms;
    uint32_t poll_interval_ms;
    QueueHandle_t notify_queue;
    
    /* State */
    button_state_t state;
    TickType_t press_start_tick;
    uint32_t press_count;
} button_task_state_t;

static button_task_state_t s_btn = {0};

/**
 * @brief Read button state from GPIO expander
 * 
 * @return true if button is pressed (active low), false otherwise
 */
static bool read_button(void)
{
    if (s_btn.gpio_expander == NULL) {
        return false;
    }
    
    aw9523_pin_data_digital_t data = false;
    esp_err_t ret = aw9523_gpio_read_pin(
        s_btn.gpio_expander,
        s_btn.button_pin,
        AW9523_PIN_GPIO_INPUT,
        &data
    );
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read button: %s", esp_err_to_name(ret));
        return false;
    }
    
    /* Button is active high (pressed = true) based on badge code */
    return data;
}

/**
 * @brief Send toggle notification to queue
 */
static void send_toggle_notification(void)
{
    if (s_btn.notify_queue == NULL) {
        ESP_LOGW(TAG, "No notify queue configured");
        return;
    }
    
    uint8_t msg = 1;
    /* Use xQueueOverwrite for length-1 queue to always succeed */
    if (xQueueOverwrite(s_btn.notify_queue, &msg) == pdTRUE) {
        ESP_LOGI(TAG, "Toggle notification sent");
    }
}

/**
 * @brief Button monitoring task
 */
static void button_task(void *arg)
{
    ESP_LOGI(TAG, "Button task started (pin %d, long press %lu ms)",
             s_btn.button_pin, (unsigned long)s_btn.long_press_ms);
    
    s_btn.running = true;
    s_btn.state = BTN_STATE_IDLE;
    
    TickType_t poll_ticks = pdMS_TO_TICKS(s_btn.poll_interval_ms);
    TickType_t long_press_ticks = pdMS_TO_TICKS(s_btn.long_press_ms);
    
    while (s_btn.running) {
        bool pressed = read_button();
        TickType_t now = xTaskGetTickCount();
        
        switch (s_btn.state) {
            case BTN_STATE_IDLE:
                if (pressed) {
                    /* Button just pressed - start timing */
                    s_btn.state = BTN_STATE_PRESSED;
                    s_btn.press_start_tick = now;
                    ESP_LOGD(TAG, "Button pressed, timing...");
                }
                break;
                
            case BTN_STATE_PRESSED:
                if (!pressed) {
                    /* Button released before long press threshold */
                    s_btn.state = BTN_STATE_IDLE;
                    ESP_LOGD(TAG, "Button released (short press)");
                } else {
                    /* Check if long press threshold reached */
                    TickType_t elapsed = now - s_btn.press_start_tick;
                    if (elapsed >= long_press_ticks) {
                        /* Long press detected! */
                        s_btn.state = BTN_STATE_LONG_FIRED;
                        s_btn.press_count++;
                        ESP_LOGI(TAG, "Long press detected! (count: %lu)",
                                 (unsigned long)s_btn.press_count);
                        send_toggle_notification();
                    }
                }
                break;
                
            case BTN_STATE_LONG_FIRED:
                if (!pressed) {
                    /* Button released after long press */
                    s_btn.state = BTN_STATE_IDLE;
                    ESP_LOGD(TAG, "Button released (after long press)");
                }
                /* While held after long press, do nothing (debounce) */
                break;
        }
        
        vTaskDelay(poll_ticks);
    }
    
    ESP_LOGI(TAG, "Button task stopped");
    vTaskDelete(NULL);
}

esp_err_t button_task_init(const button_task_config_t *config)
{
    if (s_btn.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (config->gpio_expander == NULL) {
        ESP_LOGE(TAG, "GPIO expander is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (config->notify_queue == NULL) {
        ESP_LOGW(TAG, "No notify queue configured - long press will only log");
    }
    
    /* Copy configuration */
    s_btn.gpio_expander = config->gpio_expander;
    s_btn.button_pin = config->button_pin;
    s_btn.long_press_ms = config->long_press_ms > 0 ? 
                          config->long_press_ms : BUTTON_TASK_LONG_PRESS_MS;
    s_btn.poll_interval_ms = config->poll_interval_ms > 0 ?
                             config->poll_interval_ms : BUTTON_TASK_POLL_MS;
    s_btn.notify_queue = config->notify_queue;
    
    s_btn.state = BTN_STATE_IDLE;
    s_btn.press_count = 0;
    s_btn.running = false;
    
    /* Ensure button pin is configured as input */
    esp_err_t ret = aw9523_set_pin(s_btn.gpio_expander, s_btn.button_pin,
                                    AW9523_PIN_GPIO_INPUT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure button pin: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Create task */
    BaseType_t task_ret = xTaskCreate(
        button_task,
        BUTTON_TASK_NAME,
        BUTTON_TASK_STACK_SIZE,
        NULL,
        BUTTON_TASK_PRIORITY,
        &s_btn.task_handle
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        return ESP_ERR_NO_MEM;
    }
    
    s_btn.initialized = true;
    ESP_LOGI(TAG, "Initialized on pin %d", s_btn.button_pin);
    
    return ESP_OK;
}

esp_err_t button_task_deinit(void)
{
    if (!s_btn.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Signal task to stop */
    s_btn.running = false;
    
    /* Give task time to exit */
    vTaskDelay(pdMS_TO_TICKS(s_btn.poll_interval_ms * 3));
    
    /* If task still exists, delete it */
    if (s_btn.task_handle != NULL) {
        vTaskDelete(s_btn.task_handle);
        s_btn.task_handle = NULL;
    }
    
    s_btn.initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
    
    return ESP_OK;
}

bool button_task_is_running(void)
{
    return s_btn.running;
}

uint32_t button_task_get_press_count(void)
{
    return s_btn.press_count;
}
