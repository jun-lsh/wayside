/**
 * @file main.c
 * @brief Example: Button-controlled buzzer mute toggle
 * 
 * Demonstrates:
 * - Buzzer initialization with mute support
 * - Button task monitoring P1_4 for 1-second hold
 * - Mute toggle via queue notification
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hnr26_badge.h"
#include "buzzer.h"
#include "button_task.h"

static const char *TAG = "main";

/* External reference to badge's I2C bus handle (from hnr26_badge.c) */
extern i2c_master_bus_handle_t hnr26_badge_bus_handle;

/* AW9523 device handle - we'll need to get this or create our own */
static aw9523_t s_gpio_expander;

void app_main(void)
{
    esp_err_t ret;
    
    ESP_LOGI(TAG, "=== Buzzer Mute Toggle Example ===");
    
    /* ========================================
     * 1. Initialize badge (GPIO expander + I2C)
     * ======================================== */
    ret = hnr26_badge_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Badge init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    /* Get GPIO expander handle for button task
     * Note: In hnr26_badge, the device is static, so we initialize our own
     * on the same I2C bus, or modify hnr26_badge to expose it */
    ret = aw9523_init(&hnr26_badge_bus_handle, 
                      AW9523_I2C_ADDR_AD0_GND_AD1_GND,
                      &s_gpio_expander);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO expander init failed: %s", esp_err_to_name(ret));
        /* Continue anyway - badge already initialized it */
    }
    
    /* ========================================
     * 2. Initialize buzzer
     * ======================================== */
    buzzer_config_t buzzer_cfg = BUZZER_CONFIG_DEFAULT();
    ret = buzzer_init(&buzzer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Buzzer init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    /* ========================================
     * 3. Initialize button task
     *    - Monitors P1_4 (pin 12)
     *    - 1 second hold triggers toggle
     *    - Sends to buzzer's toggle queue
     * ======================================== */
    button_task_config_t btn_cfg = {
        .gpio_expander = &s_gpio_expander,
        .button_pin = 12,                        /* P1_4 = pin 12 */
        .long_press_ms = 1000,                   /* 1 second hold */
        .poll_interval_ms = 20,                  /* 20ms polling */
        .notify_queue = buzzer_get_toggle_queue() /* Direct to buzzer */
    };
    
    ret = button_task_init(&btn_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Button task init failed: %s", esp_err_to_name(ret));
        /* Continue anyway - buzzer still works */
    }
    
    ESP_LOGI(TAG, "System initialized!");
    ESP_LOGI(TAG, "Hold P1_4 button for 1 second to toggle buzzer mute");
    
    /* ========================================
     * 4. Demo: Play beeps periodically
     * ======================================== */
    int beep_count = 0;
    while (1) {
        /* Try to beep every 3 seconds */
        vTaskDelay(pdMS_TO_TICKS(3000));
        
        beep_count++;
        ESP_LOGI(TAG, "Beep attempt #%d (muted: %s)", 
                 beep_count, buzzer_is_muted() ? "YES" : "NO");
        
        /* This will be silently ignored if muted */
        buzzer_beep(100, 100, 2);  /* Two short beeps */
        
        /* Show long press count */
        ESP_LOGI(TAG, "Long press count: %lu", 
                 (unsigned long)button_task_get_press_count());
    }
}
