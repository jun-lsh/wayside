/*
 * main.c - Main Application Entry Point
 *
 * BLE Combined Advertising and Scanning with NFC I2C Interface
 *
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "services.h"
#include "drivers/nfc.h"
#include "definitions.h"

static const char *TAG = "MAIN";

#define BUILD_DATE      __DATE__
#define BUILD_TIME      __TIME__
#define BUILD_TIMESTAMP BUILD_DATE " " BUILD_TIME

const char *build_date = BUILD_DATE;
const char *build_time = BUILD_TIME;
const char *build_timestamp = BUILD_TIMESTAMP;


static nfc_handle_t nfc_handle;
i2c_master_bus_handle_t i2c_bus = NULL;
i2c_master_dev_handle_t nfc_i2c_dev = NULL;

static esp_err_t gpio_init(void);
static esp_err_t nvs_init(void);
static void nfc_task(void *pvParameters);


/**
 * @brief FD interrupt callback (called from ISR context!)
 */
static void IRAM_ATTR nfc_fd_isr_callback(void *arg)
{
    /* Keep this minimal - just set a flag or use task notification */
    /* The task notification is already handled by the driver */
}


/**
 * @brief Initialize GPIO pins
 * 
 * Configures GP0 as output and sets it HIGH after boot.
 */
static esp_err_t gpio_init(void)
{
    esp_err_t ret;
    
    /* Configure GP0 as output */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << NFC_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GP0: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Set GP0 HIGH */
    ret = gpio_set_level(NFC_PWR_PIN, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set GP0 high: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "GP0 configured as output and set HIGH");
    return ESP_OK;
}


/**
 * @brief Initialize Non-Volatile Storage
 * 
 * Required for PHY calibration data and BLE operation.
 */
static esp_err_t nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased, reinitializing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized successfully");
    }
    
    return ret;
}


/**
 * @brief NFC task - waits for FD interrupts and handles NFC events
 * 
 * This task demonstrates using task notifications to wait for FD interrupts.
 */
static void nfc_task(void *pvParameters)
{
    esp_err_t ret;
    bool rf_present;
    uint8_t block_data[NFC_BLOCK_SIZE];
    
    ESP_LOGI(TAG, "NFC task started");
    
    /* Register this task to receive FD interrupt notifications */
    nfc_set_fd_notify_task(&nfc_handle, xTaskGetCurrentTaskHandle());
    
    while (1) {
        /* Wait for FD interrupt or timeout after 5 seconds */
        bool got_interrupt = nfc_wait_fd_interrupt(&nfc_handle, 5000);
        
        if (got_interrupt) {
            ESP_LOGI(TAG, "NFC: FD interrupt! (count: %lu)", 
                     (unsigned long)nfc_get_fd_int_count(&nfc_handle));
            
            /* Check what triggered the interrupt */
            ret = nfc_is_rf_field_present(&nfc_handle, &rf_present);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "NFC: RF field %s", 
                         rf_present ? "detected" : "removed");
            }
        }
        
        /* Periodic status check */
        bool fd_state = nfc_read_fd_pin(&nfc_handle);
        ESP_LOGD(TAG, "NFC FD pin state: %s", fd_state ? "HIGH" : "LOW");
        
        /* Example: Read block 1 (user memory starts here) */
        ret = nfc_read_block(&nfc_handle, 0x01, block_data);
        if (ret == ESP_OK) {
            ESP_LOGD(TAG, "Block 0x01 data: %02X %02X %02X %02X...", 
                     block_data[0], block_data[1], block_data[2], block_data[3]);
        }
    }
}


void app_main(void)
{
    esp_err_t ret;
    
    ESP_LOGI(TAG, "(start)");
    ESP_LOGI(TAG, "Build: %s", BUILD_TIMESTAMP);
    
    // init the gpio states (to power the nfc chip as well)
    ret = gpio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO initialization failed");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(100));  // wait for power to stabilize
    
    /* Configure I2C master bus */
    i2c_master_bus_config_t bus_config = {
        .i2c_port = NFC_I2C_PORT,
        .sda_io_num = NFC_SDA_PIN,
        .scl_io_num = NFC_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    
    ret = i2c_new_master_bus(&bus_config, &i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return;
    }
    
    /* Add NFC device to bus */
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = NFC_I2C_ADDR_DEFAULT,
        .scl_speed_hz = NFC_I2C_FREQ_HZ,
    };
    
    ret = i2c_master_bus_add_device(i2c_bus, &dev_config, &nfc_i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(i2c_bus);
        return;
    }
    
    ESP_LOGI(TAG, "I2C master initialized (SDA: GPIO%d, SCL: GPIO%d, %lu Hz)", 
             NFC_SDA_PIN, NFC_SCL_PIN, (unsigned long)NFC_I2C_FREQ_HZ);

    /* Initialize NFC driver with pin configuration */
    ret = nfc_init(&nfc_handle, nfc_i2c_dev, NFC_FD_PIN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NFC initialization failed: %s", esp_err_to_name(ret));
        return;
    }
    
    /* register FD interrupt callback (called from ISR context) */
    nfc_set_fd_callback(&nfc_handle, nfc_fd_isr_callback, NULL);
    
    // init nt3h2111 nfc device
    /* Read and display session registers to verify communication */
    nfc_session_regs_t session_regs;
    ret = nfc_read_session_registers(&nfc_handle, &session_regs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NFC session registers:");
        ESP_LOGI(TAG, "  NC_REG:    0x%02X", session_regs.nc_reg);
        ESP_LOGI(TAG, "  NS_REG:    0x%02X (RF field: %s)", 
                 session_regs.ns_reg,
                 (session_regs.ns_reg & NFC_NS_RF_FIELD_PRESENT) ? "present" : "not detected");
    } else {
        ESP_LOGW(TAG, "Failed to read NFC session registers: %s", esp_err_to_name(ret));
    }

    // init nvs for ble phy calibration
    ret = nvs_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed");
        return;
    }
    

    /* Start NFC task (uses FD interrupt notifications) */
    //xTaskCreatePinnedToCore(nfc_task, "nfc_task", 4096, NULL, 5, NULL, 0);

    // // ble scan periodic task
    // const esp_timer_create_args_t periodic_timer_args = {
    //     .callback = &periodic_timer_callback,
    //     .name = "periodic"
    // };
    
    // esp_timer_handle_t periodic_timer;
    // ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    // ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 5000000));  /* 5 seconds */
    
    
    // // bt nonsense
    // ret = bt_init();
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "Bluetooth initialization failed");
    //     return;
    // }
    
    // bt_send_commands();
    // // start bt hci event processing task
    // xTaskCreatePinnedToCore(&hci_evt_process, "hci_evt_process", 2048, NULL, 6, NULL, 0);
    
    ESP_LOGI(TAG, "(running)");
    return;
}