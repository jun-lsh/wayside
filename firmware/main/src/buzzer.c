/**
 * @file buzzer_fuet7525.c
 * @brief Driver implementation for FUET-7525-3.6V Magnetic Buzzer on ESP32
 */

#include "buzzer.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "buzzer";

#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_DUTY_RES       LEDC_TIMER_10_BIT  /* 10-bit resolution: 0-1023 */
#define LEDC_MAX_DUTY       ((1 << LEDC_DUTY_RES) - 1)  /* 1023 */

#define BUZZER_TASK_STACK_SIZE  2048
#define BUZZER_TASK_PRIORITY    5
#define BUZZER_TASK_NAME        "buzzer_task"

typedef enum {
    BUZZER_CMD_NONE = 0,
    BUZZER_CMD_START,
    BUZZER_CMD_STOP,
    BUZZER_CMD_BEEP,
    BUZZER_CMD_SEQUENCE,
} buzzer_cmd_t;

typedef struct {
    uint32_t on_ms;
    uint32_t off_ms;
    uint32_t count;     /* 0 = infinite */
} beep_pattern_t;

typedef struct {
    const uint32_t *frequencies;
    const uint32_t *durations;
    size_t length;
    size_t current_index;
} sequence_t;

typedef struct {
    bool initialized;
    bool playing;
    int gpio_num;
    uint32_t frequency;
    uint8_t volume;             /* 0-100 */
    uint32_t current_duty;      /* Actual PWM duty */
    
    TaskHandle_t task_handle;
    SemaphoreHandle_t mutex;
    
    buzzer_cmd_t cmd;
    beep_pattern_t beep;
    sequence_t sequence;
} buzzer_state_t;

static buzzer_state_t s_buzzer = {0};

static void buzzer_task(void *arg);
static uint32_t volume_to_duty(uint8_t volume);
static esp_err_t pwm_set_duty(uint32_t duty);
static esp_err_t pwm_set_frequency(uint32_t freq_hz);

/**
 * @brief Convert volume (0-100) to PWM duty cycle
 * 
 * The buzzer datasheet specifies 50% duty cycle for rated operation.
 * We map volume 0-100 to duty 0-50% (0 to LEDC_MAX_DUTY/2).
 * This keeps within safe operating range while providing volume control.
 */
static uint32_t volume_to_duty(uint8_t volume)
{
    if (volume > 100) volume = 100;
    
    /* Map 0-100 to 0-50% duty (50% = rated operation per datasheet) */
    /* duty = (volume / 100) * (max_duty / 2) */
    return (uint32_t)((volume * LEDC_MAX_DUTY) / 200);
}

/**
 * @brief Set PWM duty cycle
 */
static esp_err_t pwm_set_duty(uint32_t duty)
{
    esp_err_t ret = ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    if (ret != ESP_OK) return ret;
    
    return ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

/**
 * @brief Set PWM frequency
 */
static esp_err_t pwm_set_frequency(uint32_t freq_hz)
{
    return ledc_set_freq(LEDC_MODE, LEDC_TIMER, freq_hz);
}

/**
 * @brief Buzzer background task
 */
static void buzzer_task(void *arg)
{
    ESP_LOGI(TAG, "Buzzer task started");
    
    while (1) {
        buzzer_cmd_t cmd;
        beep_pattern_t beep = {0};
        
        /* Get current command with mutex protection */
        if (xSemaphoreTake(s_buzzer.mutex, portMAX_DELAY) == pdTRUE) {
            cmd = s_buzzer.cmd;
            if (cmd == BUZZER_CMD_BEEP) {
                memcpy(&beep, &s_buzzer.beep, sizeof(beep_pattern_t));
            }
            xSemaphoreGive(s_buzzer.mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        switch (cmd) {
            case BUZZER_CMD_START:
                /* Continuous tone - just maintain duty cycle */
                if (!s_buzzer.playing) {
                    pwm_set_duty(s_buzzer.current_duty);
                    s_buzzer.playing = true;
                    ESP_LOGD(TAG, "Started continuous tone");
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
                
            case BUZZER_CMD_STOP:
                if (s_buzzer.playing) {
                    pwm_set_duty(0);
                    s_buzzer.playing = false;
                    ESP_LOGD(TAG, "Stopped");
                }
                /* Clear command after processing */
                if (xSemaphoreTake(s_buzzer.mutex, portMAX_DELAY) == pdTRUE) {
                    if (s_buzzer.cmd == BUZZER_CMD_STOP) {
                        s_buzzer.cmd = BUZZER_CMD_NONE;
                    }
                    xSemaphoreGive(s_buzzer.mutex);
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
                
            case BUZZER_CMD_BEEP: {
                uint32_t remaining = beep.count;
                bool infinite = (beep.count == 0);
                
                while (infinite || remaining > 0) {
                    /* Check if command changed */
                    if (xSemaphoreTake(s_buzzer.mutex, 0) == pdTRUE) {
                        if (s_buzzer.cmd != BUZZER_CMD_BEEP) {
                            xSemaphoreGive(s_buzzer.mutex);
                            break;
                        }
                        xSemaphoreGive(s_buzzer.mutex);
                    }
                    
                    /* Tone ON */
                    pwm_set_duty(s_buzzer.current_duty);
                    s_buzzer.playing = true;
                    vTaskDelay(pdMS_TO_TICKS(beep.on_ms));
                    
                    /* Tone OFF */
                    pwm_set_duty(0);
                    s_buzzer.playing = false;
                    
                    if (!infinite) remaining--;
                    
                    if (remaining > 0 || infinite) {
                        vTaskDelay(pdMS_TO_TICKS(beep.off_ms));
                    }
                }
                
                /* Clear command after beep sequence completes */
                if (xSemaphoreTake(s_buzzer.mutex, portMAX_DELAY) == pdTRUE) {
                    if (s_buzzer.cmd == BUZZER_CMD_BEEP) {
                        s_buzzer.cmd = BUZZER_CMD_NONE;
                    }
                    xSemaphoreGive(s_buzzer.mutex);
                }
                break;
            }
            
            case BUZZER_CMD_SEQUENCE: {
                size_t len = 0;
                
                if (xSemaphoreTake(s_buzzer.mutex, portMAX_DELAY) == pdTRUE) {
                    len = s_buzzer.sequence.length;
                    xSemaphoreGive(s_buzzer.mutex);
                }
                
                for (size_t i = 0; i < len; i++) {
                    /* Check if command changed */
                    if (xSemaphoreTake(s_buzzer.mutex, 0) == pdTRUE) {
                        if (s_buzzer.cmd != BUZZER_CMD_SEQUENCE) {
                            xSemaphoreGive(s_buzzer.mutex);
                            break;
                        }
                        xSemaphoreGive(s_buzzer.mutex);
                    }
                    
                    uint32_t freq = s_buzzer.sequence.frequencies[i];
                    uint32_t dur = s_buzzer.sequence.durations[i];
                    
                    if (freq > 0) {
                        pwm_set_frequency(freq);
                        pwm_set_duty(s_buzzer.current_duty);
                        s_buzzer.playing = true;
                    } else {
                        /* Rest (frequency = 0) */
                        pwm_set_duty(0);
                        s_buzzer.playing = false;
                    }
                    
                    vTaskDelay(pdMS_TO_TICKS(dur));
                }
                
                pwm_set_frequency(s_buzzer.frequency);
                pwm_set_duty(0);
                s_buzzer.playing = false;
                
                if (xSemaphoreTake(s_buzzer.mutex, portMAX_DELAY) == pdTRUE) {
                    if (s_buzzer.cmd == BUZZER_CMD_SEQUENCE) {
                        s_buzzer.cmd = BUZZER_CMD_NONE;
                    }
                    xSemaphoreGive(s_buzzer.mutex);
                }
                break;
            }
                
            case BUZZER_CMD_NONE:
            default:
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
        }
    }
}

esp_err_t buzzer_init(const buzzer_config_t *config)
{
    if (s_buzzer.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (config) {
        s_buzzer.gpio_num = config->gpio_num;
        s_buzzer.frequency = config->frequency > 0 ? config->frequency : BUZZER_FREQ_HZ;
        s_buzzer.volume = config->initial_volume <= 100 ? config->initial_volume : BUZZER_VOLUME_DEFAULT;
    } else {
        s_buzzer.gpio_num = BUZZER_DEFAULT_GPIO;
        s_buzzer.frequency = BUZZER_FREQ_HZ;
        s_buzzer.volume = BUZZER_VOLUME_DEFAULT;
    }
    
    s_buzzer.current_duty = volume_to_duty(s_buzzer.volume);
    
    ESP_LOGI(TAG, "Initializing on GPIO %d, freq %lu Hz, volume %d%%",
             s_buzzer.gpio_num, (unsigned long)s_buzzer.frequency, s_buzzer.volume);
    
    /* Configure LEDC timer */
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = s_buzzer.frequency,
        .clk_cfg = LEDC_AUTO_CLK
    };
    esp_err_t ret = ledc_timer_config(&timer_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Timer config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ledc_channel_config_t channel_conf = {
        .gpio_num = s_buzzer.gpio_num,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0
    };
    ret = ledc_channel_config(&channel_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    s_buzzer.mutex = xSemaphoreCreateMutex();
    if (s_buzzer.mutex == NULL) {
        ESP_LOGE(TAG, "Mutex creation failed");
        return ESP_ERR_NO_MEM;
    }
    
    BaseType_t task_ret = xTaskCreate(
        buzzer_task,
        BUZZER_TASK_NAME,
        BUZZER_TASK_STACK_SIZE,
        NULL,
        BUZZER_TASK_PRIORITY,
        &s_buzzer.task_handle
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Task creation failed");
        vSemaphoreDelete(s_buzzer.mutex);
        return ESP_ERR_NO_MEM;
    }
    
    s_buzzer.initialized = true;
    s_buzzer.playing = false;
    s_buzzer.cmd = BUZZER_CMD_NONE;
    
    ESP_LOGI(TAG, "Initialized successfully");
    return ESP_OK;
}

esp_err_t buzzer_deinit(void)
{
    if (!s_buzzer.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    buzzer_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    if (s_buzzer.task_handle) {
        vTaskDelete(s_buzzer.task_handle);
        s_buzzer.task_handle = NULL;
    }
    
    if (s_buzzer.mutex) {
        vSemaphoreDelete(s_buzzer.mutex);
        s_buzzer.mutex = NULL;
    }
    
    ledc_stop(LEDC_MODE, LEDC_CHANNEL, 0);
    
    s_buzzer.initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
    
    return ESP_OK;
}

esp_err_t buzzer_start(void)
{
    if (!s_buzzer.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_buzzer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_buzzer.cmd = BUZZER_CMD_START;
        xSemaphoreGive(s_buzzer.mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t buzzer_stop(void)
{
    if (!s_buzzer.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_buzzer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_buzzer.cmd = BUZZER_CMD_STOP;
        xSemaphoreGive(s_buzzer.mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

bool buzzer_is_playing(void)
{
    return s_buzzer.playing;
}

esp_err_t buzzer_set_volume(uint8_t volume)
{
    if (!s_buzzer.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (volume > BUZZER_VOLUME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_buzzer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_buzzer.volume = volume;
        s_buzzer.current_duty = volume_to_duty(volume);
        
        if (s_buzzer.playing) {
            pwm_set_duty(s_buzzer.current_duty);
        }
        
        xSemaphoreGive(s_buzzer.mutex);
        ESP_LOGD(TAG, "Volume set to %d%% (duty: %lu)", volume, (unsigned long)s_buzzer.current_duty);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

uint8_t buzzer_get_volume(void)
{
    return s_buzzer.volume;
}

esp_err_t buzzer_volume_up(void)
{
    uint8_t new_vol = s_buzzer.volume + BUZZER_VOLUME_STEP;
    if (new_vol > BUZZER_VOLUME_MAX) {
        new_vol = BUZZER_VOLUME_MAX;
    }
    return buzzer_set_volume(new_vol);
}

esp_err_t buzzer_volume_down(void)
{
    uint8_t new_vol;
    if (s_buzzer.volume < BUZZER_VOLUME_STEP) {
        new_vol = BUZZER_VOLUME_MIN;
    } else {
        new_vol = s_buzzer.volume - BUZZER_VOLUME_STEP;
    }
    return buzzer_set_volume(new_vol);
}

esp_err_t buzzer_set_frequency(uint32_t freq_hz)
{
    if (!s_buzzer.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (freq_hz < 100 || freq_hz > 20000) {
        ESP_LOGW(TAG, "Frequency %lu Hz out of typical range", (unsigned long)freq_hz);
    }
    
    if (xSemaphoreTake(s_buzzer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_buzzer.frequency = freq_hz;
        pwm_set_frequency(freq_hz);
        xSemaphoreGive(s_buzzer.mutex);
        ESP_LOGD(TAG, "Frequency set to %lu Hz", (unsigned long)freq_hz);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

uint32_t buzzer_get_frequency(void)
{
    return s_buzzer.frequency;
}

esp_err_t buzzer_beep(uint32_t on_ms, uint32_t off_ms, uint32_t count)
{
    if (!s_buzzer.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(s_buzzer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_buzzer.beep.on_ms = on_ms;
        s_buzzer.beep.off_ms = off_ms;
        s_buzzer.beep.count = count;
        s_buzzer.cmd = BUZZER_CMD_BEEP;
        xSemaphoreGive(s_buzzer.mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}

esp_err_t buzzer_beep_once(void)
{
    return buzzer_beep(100, 0, 1);
}

esp_err_t buzzer_play_sequence(const uint32_t *frequencies, 
                                const uint32_t *durations, 
                                size_t length)
{
    if (!s_buzzer.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (frequencies == NULL || durations == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(s_buzzer.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_buzzer.sequence.frequencies = frequencies;
        s_buzzer.sequence.durations = durations;
        s_buzzer.sequence.length = length;
        s_buzzer.cmd = BUZZER_CMD_SEQUENCE;
        xSemaphoreGive(s_buzzer.mutex);
        return ESP_OK;
    }
    
    return ESP_ERR_TIMEOUT;
}
