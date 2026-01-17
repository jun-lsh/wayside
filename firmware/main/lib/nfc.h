/*
 * nfc.h - NT3H2111/NT3H2211 NFC I2C Driver
 *
 * Driver for NXP NTAG I2C plus NFC chip
 * Based on NT3H2111_2211 datasheet Rev. 3.6
 */

#ifndef NFC_H
#define NFC_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * I2C Configuration
 * ============================================================================ */

#define NFC_I2C_PORT            I2C_NUM_0
#define NFC_I2C_TIMEOUT_MS      1000

/* Default I2C device address:
 *   7-bit slave address: 0x55
 *   8-bit write address: 0xAA (0x55 << 1 | 0)
 *   8-bit read address:  0xAB (0x55 << 1 | 1)
 * 
 * ESP-IDF uses 7-bit addressing - the driver shifts and adds R/W bit.
 */
#define NFC_I2C_ADDR_DEFAULT    0x55

/* ============================================================================
 * NT3H2111/2211 Memory Map
 * ============================================================================ */

/* EEPROM memory blocks */
#define NFC_BLOCK_SIZE          16           /* Each block is 16 bytes */

/* NT3H2111 (1k) memory range: 0x00 to 0x3A */
#define NFC_1K_EEPROM_START     0x00
#define NFC_1K_EEPROM_END       0x3A

/* NT3H2211 (2k) memory range: 0x00 to 0x7A */
#define NFC_2K_EEPROM_START     0x00
#define NFC_2K_EEPROM_END       0x7A

/* SRAM memory range (both variants) */
#define NFC_SRAM_START          0xF8
#define NFC_SRAM_END            0xFB

/* Session registers block */
#define NFC_SESSION_REG_BLOCK   0xFE

/* Configuration block */
#define NFC_CONFIG_BLOCK        0x3A         /* For 1k variant */

/* ============================================================================
 * Session Register Addresses (within block 0xFE)
 * ============================================================================ */

#define NFC_REG_NC_REG          0x00         /* NC_REG - NFC configuration */
#define NFC_REG_LAST_NDEF_BLOCK 0x01         /* Last NDEF block */
#define NFC_REG_SRAM_MIRROR_BLK 0x02         /* SRAM mirror block */
#define NFC_REG_WDT_LS          0x03         /* Watchdog timer LS */
#define NFC_REG_WDT_MS          0x04         /* Watchdog timer MS */
#define NFC_REG_I2C_CLOCK_STR   0x05         /* I2C clock stretching */
#define NFC_REG_NS_REG          0x06         /* NS_REG - NFC status */
#define NFC_REG_FIXED           0x07         /* Fixed value (read-only) */

/* ============================================================================
 * NS_REG (Status Register) Bit Definitions
 * ============================================================================ */

#define NFC_NS_NDEF_DATA_READ   (1 << 7)     /* NDEF data read by RF */
#define NFC_NS_I2C_LOCKED       (1 << 6)     /* I2C locked */
#define NFC_NS_RF_LOCKED        (1 << 5)     /* RF locked */
#define NFC_NS_SRAM_I2C_READY   (1 << 4)     /* SRAM ready for I2C */
#define NFC_NS_SRAM_RF_READY    (1 << 3)     /* SRAM ready for RF */
#define NFC_NS_EEPROM_WR_ERR    (1 << 2)     /* EEPROM write error */
#define NFC_NS_EEPROM_WR_BUSY   (1 << 1)     /* EEPROM write busy */
#define NFC_NS_RF_FIELD_PRESENT (1 << 0)     /* RF field detected */

/* ============================================================================
 * NC_REG (Configuration Register) Bit Definitions
 * ============================================================================ */

#define NFC_NC_NFCS_I2C_RST     (1 << 7)     /* I2C soft reset / NFC disable */
#define NFC_NC_PTHRU_ON_OFF     (1 << 6)     /* Pass-through mode */
#define NFC_NC_FD_OFF           (0 << 4)     /* FD pin: field detection */
#define NFC_NC_FD_ON            (1 << 4)     /* FD pin options */
#define NFC_NC_SRAM_MIRROR      (1 << 3)     /* SRAM mirror mode */
#define NFC_NC_TRANSFER_DIR     (1 << 0)     /* Transfer direction */

/* ============================================================================
 * Timing Constants
 * ============================================================================ */

#define NFC_EEPROM_WRITE_TIME_MS    4        /* EEPROM write cycle time */
#define NFC_READ_DELAY_US           50       /* Delay before read start */

/* ============================================================================
 * Interrupt Configuration
 * ============================================================================ */

#define NFC_FD_INTR_TYPE        GPIO_INTR_NEGEDGE  /* Falling edge trigger */

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/**
 * @brief FD interrupt callback type
 * 
 * @param arg User-provided argument
 */
typedef void (*nfc_fd_callback_t)(void *arg);

/**
 * @brief NFC device handle structure
 */
typedef struct {
    i2c_master_bus_handle_t i2c_bus;    /* I2C bus handle */
    i2c_master_dev_handle_t i2c_dev;    /* I2C device handle */
    gpio_num_t fd_pin;                  /* Field Detect pin */
    bool initialized;                   /* Initialization flag */
    TaskHandle_t notify_task;           /* Task to notify on FD interrupt */
    nfc_fd_callback_t fd_callback;      /* Optional callback for FD interrupt */
    void *fd_callback_arg;              /* Argument for FD callback */
    volatile uint32_t fd_int_count;     /* FD interrupt counter */
} nfc_handle_t;

/**
 * @brief NFC session register structure
 */
typedef struct {
    uint8_t nc_reg;             /* NC_REG */
    uint8_t last_ndef_block;    /* Last NDEF block */
    uint8_t sram_mirror_block;  /* SRAM mirror block */
    uint8_t wdt_ls;             /* Watchdog timer LS */
    uint8_t wdt_ms;             /* Watchdog timer MS */
    uint8_t i2c_clock_str;      /* I2C clock stretching */
    uint8_t ns_reg;             /* NS_REG (status) */
    uint8_t fixed;              /* Fixed value */
} nfc_session_regs_t;

/* ============================================================================
 * Function Prototypes
 * ============================================================================ */

/**
 * @brief Initialize the NFC I2C driver
 * 
 * @param handle Pointer to NFC handle structure
 * @param dev i2c master device handle
 * @param fd_pin GPIO pin for Field Detect interrupt
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_init(nfc_handle_t *handle, i2c_master_dev_handle_t dev, gpio_num_t fd_pin);

/**
 * @brief Deinitialize the NFC driver
 * 
 * @param handle Pointer to NFC handle structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_deinit(nfc_handle_t *handle);

/**
 * @brief Set task to receive FD interrupt notifications
 * 
 * When an FD interrupt occurs, the specified task will receive a 
 * task notification (using xTaskNotifyFromISR).
 * 
 * @param handle Pointer to NFC handle structure
 * @param task Task handle to notify (use xTaskGetCurrentTaskHandle() for current task)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_set_fd_notify_task(nfc_handle_t *handle, TaskHandle_t task);

/**
 * @brief Register callback for FD interrupt
 * 
 * The callback will be called from ISR context - keep it short!
 * 
 * @param handle Pointer to NFC handle structure
 * @param callback Callback function (called from ISR context)
 * @param arg User argument passed to callback
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_set_fd_callback(nfc_handle_t *handle, nfc_fd_callback_t callback, void *arg);

/**
 * @brief Wait for FD interrupt with timeout
 * 
 * Blocks until FD interrupt occurs or timeout expires.
 * Requires nfc_set_fd_notify_task() to be called first.
 * 
 * @param handle Pointer to NFC handle structure
 * @param timeout_ms Timeout in milliseconds (portMAX_DELAY for infinite)
 * @return true if interrupt occurred, false if timeout
 */
bool nfc_wait_fd_interrupt(nfc_handle_t *handle, uint32_t timeout_ms);

/**
 * @brief Get FD interrupt count
 * 
 * @param handle Pointer to NFC handle structure
 * @return Number of FD interrupts since initialization
 */
uint32_t nfc_get_fd_int_count(nfc_handle_t *handle);

/**
 * @brief Clear FD interrupt count
 * 
 * @param handle Pointer to NFC handle structure
 */
void nfc_clear_fd_int_count(nfc_handle_t *handle);

/**
 * @brief Read a 16-byte block from EEPROM or SRAM
 * 
 * @param handle Pointer to NFC handle structure
 * @param block_addr Block address (0x00-0x3A for 1k, 0x00-0x7A for 2k, 0xF8-0xFB for SRAM)
 * @param data Buffer to store read data (must be at least 16 bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_read_block(nfc_handle_t *handle, uint8_t block_addr, uint8_t *data);

/**
 * @brief Write a 16-byte block to EEPROM or SRAM
 * 
 * Note: EEPROM write requires ~4ms programming time after this function returns.
 * 
 * @param handle Pointer to NFC handle structure
 * @param block_addr Block address (0x00-0x3A for 1k, 0x00-0x7A for 2k, 0xF8-0xFB for SRAM)
 * @param data Data to write (must be 16 bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_write_block(nfc_handle_t *handle, uint8_t block_addr, const uint8_t *data);

/**
 * @brief Read a session register byte
 * 
 * @param handle Pointer to NFC handle structure
 * @param reg_addr Register address within session block (0x00-0x07)
 * @param value Pointer to store read value
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_read_register(nfc_handle_t *handle, uint8_t reg_addr, uint8_t *value);

/**
 * @brief Write a session register byte with mask
 * 
 * Only bits set in the mask will be modified.
 * 
 * @param handle Pointer to NFC handle structure
 * @param reg_addr Register address within session block (0x00-0x07)
 * @param mask Bit mask (1 = modify this bit, 0 = keep existing)
 * @param value New value for masked bits
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_write_register(nfc_handle_t *handle, uint8_t reg_addr, uint8_t mask, uint8_t value);

/**
 * @brief Read all session registers
 * 
 * @param handle Pointer to NFC handle structure
 * @param regs Pointer to session registers structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_read_session_registers(nfc_handle_t *handle, nfc_session_regs_t *regs);

/**
 * @brief Check if RF field is present
 * 
 * @param handle Pointer to NFC handle structure
 * @param present Pointer to store result (true if field detected)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_is_rf_field_present(nfc_handle_t *handle, bool *present);

/**
 * @brief Check if EEPROM write is in progress
 * 
 * @param handle Pointer to NFC handle structure
 * @param busy Pointer to store result (true if write in progress)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_is_write_busy(nfc_handle_t *handle, bool *busy);

/**
 * @brief Wait for EEPROM write to complete
 * 
 * @param handle Pointer to NFC handle structure
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if timeout
 */
esp_err_t nfc_wait_write_complete(nfc_handle_t *handle, uint32_t timeout_ms);

/**
 * @brief Read the Field Detect (FD) pin state
 * 
 * @param handle Pointer to NFC handle structure
 * @return true if FD pin is high, false if low
 */
bool nfc_read_fd_pin(nfc_handle_t *handle);

/**
 * @brief Read multiple consecutive blocks
 * 
 * @param handle Pointer to NFC handle structure
 * @param start_block Starting block address
 * @param num_blocks Number of blocks to read
 * @param data Buffer to store read data (must be at least num_blocks * 16 bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_read_blocks(nfc_handle_t *handle, uint8_t start_block, uint8_t num_blocks, uint8_t *data);

/**
 * @brief Write multiple consecutive blocks
 * 
 * @param handle Pointer to NFC handle structure
 * @param start_block Starting block address
 * @param num_blocks Number of blocks to write
 * @param data Data to write (must be num_blocks * 16 bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nfc_write_blocks(nfc_handle_t *handle, uint8_t start_block, uint8_t num_blocks, const uint8_t *data);

/**
 * @brief Scan I2C bus for devices (debug helper)
 * 
 * Scans all 127 possible I2C addresses and reports found devices.
 * Useful for debugging wiring issues.
 * 
 * @param handle Pointer to initialized NFC handle structure
 */
void nfc_i2c_scan(nfc_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* NFC_H */
