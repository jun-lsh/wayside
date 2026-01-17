/*
 * nfc.c - NT3H2111/NT3H2211 NFC I2C Driver Implementation
 *
 * Driver for NXP NTAG I2C plus NFC chip
 * Based on NT3H2111_2211 datasheet Rev. 3.6
 * 
 * Uses ESP-IDF 5.x i2c_master.h API
 */

#include "nfc.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "NFC_DRV";

/* ============================================================================
 * ISR Handler
 * ============================================================================ */

/**
 * @brief FD pin interrupt service routine
 */
static void IRAM_ATTR nfc_fd_isr_handler(void *arg)
{
    nfc_handle_t *handle = (nfc_handle_t *)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    /* Increment interrupt counter */
    handle->fd_int_count++;
    
    /* Call user callback if registered */
    if (handle->fd_callback != NULL) {
        handle->fd_callback(handle->fd_callback_arg);
    }
    
    /* Notify task if registered */
    if (handle->notify_task != NULL) {
        vTaskNotifyGiveFromISR(handle->notify_task, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/* ============================================================================
 * Private Helper Functions
 * ============================================================================ */

/**
 * @brief Configure FD GPIO pin with interrupt
 */
static esp_err_t nfc_configure_fd_gpio(nfc_handle_t *handle)
{
    esp_err_t ret;
    
    /* Configure FD pin as input with pullup */
    gpio_config_t fd_conf = {
        .pin_bit_mask = (1ULL << handle->fd_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = NFC_FD_INTR_TYPE
    };
    
    ret = gpio_config(&fd_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure FD pin: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Install GPIO ISR service (if not already installed) */
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means ISR service already installed - that's OK */
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Attach ISR handler */
    ret = gpio_isr_handler_add(handle->fd_pin, nfc_fd_isr_handler, (void *)handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "FD pin (GPIO%d) configured with interrupt (falling edge)", handle->fd_pin);
    return ESP_OK;
}

esp_err_t nfc_init(nfc_handle_t *handle, i2c_master_dev_handle_t dev, gpio_num_t fd_pin)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    
    /* Initialize handle structure */
    memset(handle, 0, sizeof(nfc_handle_t));
    handle->i2c_dev = dev;
    handle->fd_pin = fd_pin;
    handle->notify_task = NULL;
    handle->fd_callback = NULL;
    handle->fd_callback_arg = NULL;
    handle->fd_int_count = 0;
    
    /* Configure FD GPIO with interrupt */
    ret = nfc_configure_fd_gpio(handle);
    if (ret != ESP_OK) {
        return ret;
    }
    
    handle->initialized = true;
    
    ESP_LOGI(TAG, "NFC driver initialized (I2C dev handle: %p, FD pin: GPIO%d)", 
             (void *)handle->i2c_dev, handle->fd_pin);
    return ESP_OK;
}

esp_err_t nfc_deinit(nfc_handle_t *handle)
{
    if (handle == NULL || !handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Remove ISR handler */
    gpio_isr_handler_remove(handle->fd_pin);
    
    /* Remove device from bus */
    i2c_master_bus_rm_device(handle->i2c_dev);
    
    /* Delete I2C bus */
    esp_err_t ret = i2c_del_master_bus(handle->i2c_bus);
    if (ret == ESP_OK) {
        handle->initialized = false;
        ESP_LOGI(TAG, "NFC driver deinitialized");
    }
    
    return ret;
}

esp_err_t nfc_set_fd_notify_task(nfc_handle_t *handle, TaskHandle_t task)
{
    if (handle == NULL || !handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    handle->notify_task = task;
    ESP_LOGI(TAG, "FD notify task set");
    return ESP_OK;
}

esp_err_t nfc_set_fd_callback(nfc_handle_t *handle, nfc_fd_callback_t callback, void *arg)
{
    if (handle == NULL || !handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    handle->fd_callback = callback;
    handle->fd_callback_arg = arg;
    ESP_LOGI(TAG, "FD callback registered");
    return ESP_OK;
}

bool nfc_wait_fd_interrupt(nfc_handle_t *handle, uint32_t timeout_ms)
{
    if (handle == NULL || !handle->initialized || handle->notify_task == NULL) {
        return false;
    }
    
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? 
                       portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    
    uint32_t notification_value = ulTaskNotifyTake(pdTRUE, ticks);
    return (notification_value > 0);
}

uint32_t nfc_get_fd_int_count(nfc_handle_t *handle)
{
    if (handle == NULL) {
        return 0;
    }
    return handle->fd_int_count;
}

void nfc_clear_fd_int_count(nfc_handle_t *handle)
{
    if (handle != NULL) {
        handle->fd_int_count = 0;
    }
}

esp_err_t nfc_read_block(nfc_handle_t *handle, uint8_t block_addr, uint8_t *data)
{
    if (handle == NULL || !handle->initialized || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    
    /* 
     * READ operation sequence (from datasheet Figure 18):
     * 1. Start + SA(W) + ACK + MEMA + ACK + Stop
     * 2. Start + SA(R) + ACK + D0..D15 + ACK (each byte) + Stop
     */
    
    /* Step 1: Send memory address */
    ret = i2c_master_transmit(handle->i2c_dev, &block_addr, 1, NFC_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read block addr write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Wait at least 50μs before read start (if I2C_CLOCK_STR is 0) */
    esp_rom_delay_us(NFC_READ_DELAY_US);
    
    /* Step 2: Read 16 bytes of data */
    ret = i2c_master_receive(handle->i2c_dev, data, NFC_BLOCK_SIZE, NFC_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read block data failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGD(TAG, "Read block 0x%02X complete", block_addr);
    return ESP_OK;
}

esp_err_t nfc_write_block(nfc_handle_t *handle, uint8_t block_addr, const uint8_t *data)
{
    if (handle == NULL || !handle->initialized || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /*
     * WRITE operation sequence (from datasheet Figure 18):
     * Start + SA(W) + ACK + MEMA + ACK + D0..D15 + ACK (each byte) + Stop
     * 
     * WARNING: Must wait ~4ms after Stop for EEPROM programming!
     */
    
    /* Build write buffer: address + 16 bytes data */
    uint8_t write_buf[1 + NFC_BLOCK_SIZE];
    write_buf[0] = block_addr;
    memcpy(&write_buf[1], data, NFC_BLOCK_SIZE);
    
    esp_err_t ret = i2c_master_transmit(handle->i2c_dev, write_buf, sizeof(write_buf), NFC_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write block failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Wait for EEPROM programming time (only needed for EEPROM, not SRAM) */
    if (block_addr < NFC_SRAM_START) {
        vTaskDelay(pdMS_TO_TICKS(NFC_EEPROM_WRITE_TIME_MS));
    }
    
    ESP_LOGD(TAG, "Write block 0x%02X complete", block_addr);
    return ESP_OK;
}

esp_err_t nfc_read_register(nfc_handle_t *handle, uint8_t reg_addr, uint8_t *value)
{
    if (handle == NULL || !handle->initialized || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (reg_addr > 0x07) {
        ESP_LOGE(TAG, "Invalid register address: 0x%02X", reg_addr);
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    
    /*
     * READ register sequence (from datasheet Figure 19):
     * 1. Start + SA(W) + ACK + MEMA(0xFE) + ACK + REGA + ACK + Stop
     * 2. Start + SA(R) + ACK + REGDAT + ACK + Stop
     */
    
    /* Step 1: Send session register block address and register address */
    uint8_t addr_buf[2] = { NFC_SESSION_REG_BLOCK, reg_addr };
    ret = i2c_master_transmit(handle->i2c_dev, addr_buf, 2, NFC_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read register addr write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Wait before read */
    esp_rom_delay_us(NFC_READ_DELAY_US);
    
    /* Step 2: Read register value */
    ret = i2c_master_receive(handle->i2c_dev, value, 1, NFC_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read register data failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGD(TAG, "Read register 0x%02X = 0x%02X", reg_addr, *value);
    return ESP_OK;
}

esp_err_t nfc_write_register(nfc_handle_t *handle, uint8_t reg_addr, uint8_t mask, uint8_t value)
{
    if (handle == NULL || !handle->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (reg_addr > 0x07) {
        ESP_LOGE(TAG, "Invalid register address: 0x%02X", reg_addr);
        return ESP_ERR_INVALID_ARG;
    }
    
    /*
     * WRITE register sequence (from datasheet Figure 19):
     * Start + SA(W) + ACK + MEMA(0xFE) + ACK + REGA + ACK + MASK + ACK + REGDAT + ACK + Stop
     */
    
    uint8_t write_buf[4] = { NFC_SESSION_REG_BLOCK, reg_addr, mask, value };
    esp_err_t ret = i2c_master_transmit(handle->i2c_dev, write_buf, 4, NFC_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write register failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGD(TAG, "Write register 0x%02X: mask=0x%02X, value=0x%02X", reg_addr, mask, value);
    return ESP_OK;
}

esp_err_t nfc_read_session_registers(nfc_handle_t *handle, nfc_session_regs_t *regs)
{
    if (handle == NULL || !handle->initialized || regs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    uint8_t *reg_array = (uint8_t *)regs;
    
    /* Read each register individually */
    for (uint8_t i = 0; i < sizeof(nfc_session_regs_t); i++) {
        ret = nfc_read_register(handle, i, &reg_array[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    
    return ESP_OK;
}

esp_err_t nfc_is_rf_field_present(nfc_handle_t *handle, bool *present)
{
    if (handle == NULL || present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t ns_reg;
    esp_err_t ret = nfc_read_register(handle, NFC_REG_NS_REG, &ns_reg);
    if (ret != ESP_OK) {
        return ret;
    }
    
    *present = (ns_reg & NFC_NS_RF_FIELD_PRESENT) != 0;
    return ESP_OK;
}

esp_err_t nfc_is_write_busy(nfc_handle_t *handle, bool *busy)
{
    if (handle == NULL || busy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t ns_reg;
    esp_err_t ret = nfc_read_register(handle, NFC_REG_NS_REG, &ns_reg);
    if (ret != ESP_OK) {
        return ret;
    }
    
    *busy = (ns_reg & NFC_NS_EEPROM_WR_BUSY) != 0;
    return ESP_OK;
}

esp_err_t nfc_wait_write_complete(nfc_handle_t *handle, uint32_t timeout_ms)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint32_t start_time = xTaskGetTickCount();
    bool busy;
    
    do {
        esp_err_t ret = nfc_is_write_busy(handle, &busy);
        if (ret != ESP_OK) {
            return ret;
        }
        
        if (!busy) {
            return ESP_OK;
        }
        
        vTaskDelay(pdMS_TO_TICKS(1));
        
    } while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(timeout_ms));
    
    ESP_LOGE(TAG, "Timeout waiting for write complete");
    return ESP_ERR_TIMEOUT;
}

bool nfc_read_fd_pin(nfc_handle_t *handle)
{
    if (handle == NULL) {
        return false;
    }
    return gpio_get_level(handle->fd_pin) != 0;
}

esp_err_t nfc_read_blocks(nfc_handle_t *handle, uint8_t start_block, uint8_t num_blocks, uint8_t *data)
{
    if (handle == NULL || !handle->initialized || data == NULL || num_blocks == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    
    for (uint8_t i = 0; i < num_blocks; i++) {
        ret = nfc_read_block(handle, start_block + i, &data[i * NFC_BLOCK_SIZE]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read block 0x%02X", start_block + i);
            return ret;
        }
    }
    
    return ESP_OK;
}

esp_err_t nfc_write_blocks(nfc_handle_t *handle, uint8_t start_block, uint8_t num_blocks, const uint8_t *data)
{
    if (handle == NULL || !handle->initialized || data == NULL || num_blocks == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    
    for (uint8_t i = 0; i < num_blocks; i++) {
        ret = nfc_write_block(handle, start_block + i, &data[i * NFC_BLOCK_SIZE]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write block 0x%02X", start_block + i);
            return ret;
        }
    }
    
    return ESP_OK;
}