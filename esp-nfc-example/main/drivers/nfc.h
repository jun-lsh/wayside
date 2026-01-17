/*
 * nfc.h - nt3h2111 nfc i2c driver
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

#ifdef __cplusplus
extern "C" {
#endif

/* i2c config */
#define NFC_I2C_ADDR            0x55
#define NFC_I2C_TIMEOUT_MS      100
#define NFC_EEPROM_WRITE_DELAY_MS 1  /* wait time after writing to eeprom */

/* memory map (i2c blocks are 16 bytes, nfc pages are 4 bytes)
 * i2c block N = nfc pages N*4 to N*4+3
 * block 0     - uid, lock bytes, cc
 * block 1-55  - user data (1k) or 1-127 (2k)
 * block 56-57 - auth config (auth0, access, pwd, pack, pt_i2c)
 * block 58    - config registers
 * block 248-251 - sram (64 bytes)
 * block 254   - session registers
 */
#define NFC_BLOCK_SIZE          16
#define NFC_SESSION_REG_BLOCK   0xFE
#define NFC_CONFIG_BLOCK        0x3A
#define NFC_SRAM_START          0xF8
#define NFC_AUTH_BLOCK          0x38    /* auth0 at byte 15 */
#define NFC_ACCESS_BLOCK        0x39    /* access, pwd, pack, pt_i2c */

/* session/config register offsets (same layout for both) */
#define NFC_REG_NC              0x00
#define NFC_REG_LAST_NDEF       0x01
#define NFC_REG_SRAM_MIRROR     0x02
#define NFC_REG_WDT_LS          0x03
#define NFC_REG_WDT_MS          0x04
#define NFC_REG_I2C_CLK_STR     0x05
#define NFC_REG_NS              0x06    /* ns_reg in session, reg_lock in config */

/* ns_reg bits */
#define NFC_NS_RF_FIELD         (1 << 0)
#define NFC_NS_EEPROM_BUSY      (1 << 1)
#define NFC_NS_SRAM_RF_READY    (1 << 4)
#define NFC_NS_RF_LOCKED        (1 << 5)
#define NFC_NS_I2C_LOCKED       (1 << 6)
#define NFC_NS_NDEF_READ        (1 << 7)

/* nc_reg bits */
#define NFC_NC_FD_OFF_SHIFT     4
#define NFC_NC_FD_OFF_MASK      (0x03 << NFC_NC_FD_OFF_SHIFT)
#define NFC_NC_FD_ON_SHIFT      2
#define NFC_NC_FD_ON_MASK       (0x03 << NFC_NC_FD_ON_SHIFT)
#define NFC_NC_SRAM_MIRROR      (1 << 1)
#define NFC_NC_PTHRU            (1 << 0)
#define NFC_NC_I2C_RST_ON_OFF   (1 << 7)
#define NFC_NC_DIR_PTHRU        (1 << 6)    /* 0=nfc->i2c, 1=i2c->nfc */

/* access block byte offsets (within block 0x39) */
#define NFC_ACCESS_BYTE         0
#define NFC_PWD_OFFSET          4
#define NFC_PACK_OFFSET         8
#define NFC_PT_I2C_OFFSET       12

/* access byte bits */
#define NFC_ACCESS_NFC_PROT     (1 << 7)    /* 0=write protect, 1=read+write */
#define NFC_ACCESS_NFC_DIS_SEC1 (1 << 5)    /* disable nfc access to sector 1 (2k only) */
#define NFC_ACCESS_AUTHLIM_MASK 0x07

/* pt_i2c byte bits */
#define NFC_PT_2K_PROT          (1 << 3)    /* password protect sector 1 (2k only) */
#define NFC_PT_SRAM_PROT        (1 << 2)    /* password protect sram in pthru */
#define NFC_PT_I2C_PROT_MASK    0x03

/* fd pin output modes (active low) */
typedef enum {
    NFC_FD_OFF_RF_OFF = 0,      /* fd high when rf field off */
    NFC_FD_OFF_LAST_NDEF = 1,   /* fd high when last ndef block read */
    NFC_FD_OFF_I2C_DONE = 2,    /* fd high when i2c write complete */
} nfc_fd_off_t;

typedef enum {
    NFC_FD_ON_RF_ON = 0,        /* fd low when rf field on */
    NFC_FD_ON_FIRST_NDEF = 1,   /* fd low when first ndef data read */
    NFC_FD_ON_I2C_LAST = 2,     /* fd low when last i2c data received */
    NFC_FD_ON_DATA_READY = 3,   /* fd low when sram data ready */
} nfc_fd_on_t;

/* i2c access protection level for protected memory area */
typedef enum {
    NFC_I2C_PROT_NONE = 0,      /* full read/write access */
    NFC_I2C_PROT_READ_ONLY = 1, /* read only for protected area */
    NFC_I2C_PROT_NO_ACCESS = 2, /* no access to protected area */
} nfc_i2c_prot_t;

/* protection configuration */
typedef struct {
    uint8_t auth0;              /* nfc page where protection starts (0xff = disabled) */
    bool nfc_read_prot;         /* true = read+write protected, false = write only */
    uint8_t authlim;            /* auth attempts: 0=unlimited, 1-7 = 2^n attempts */
    nfc_i2c_prot_t i2c_prot;    /* i2c access level for protected area */
    bool sram_prot;             /* protect sram in pass-through mode */
    uint8_t pwd[4];             /* 4-byte password */
    uint8_t pack[2];            /* 2-byte password acknowledge */
} nfc_prot_cfg_t;

/* fd interrupt callback */
typedef void (*nfc_fd_cb_t)(void *arg);

/* handle */
typedef struct {
    i2c_master_dev_handle_t dev;
    gpio_num_t fd_pin;
    TaskHandle_t notify_task;
    nfc_fd_cb_t fd_cb;
    void *fd_cb_arg;
    volatile uint32_t fd_count;
} nfc_t;

/* init/deinit */
esp_err_t nfc_init(nfc_t *nfc, i2c_master_bus_handle_t bus, uint8_t addr, uint32_t freq_hz, gpio_num_t fd_pin);
esp_err_t nfc_deinit(nfc_t *nfc);

/* block read/write (16 bytes) */
esp_err_t nfc_read_block(nfc_t *nfc, uint8_t block, uint8_t *data, bool release_lock);
esp_err_t nfc_write_block(nfc_t *nfc, uint8_t block, const uint8_t *data, bool release_lock);

/* session register read/write */
esp_err_t nfc_read_reg(nfc_t *nfc, uint8_t reg, uint8_t *val);
esp_err_t nfc_write_reg(nfc_t *nfc, uint8_t reg, uint8_t mask, uint8_t val);

/* status helpers */
esp_err_t nfc_get_ns_reg(nfc_t *nfc, uint8_t *ns);
esp_err_t nfc_get_nc_reg(nfc_t *nfc, uint8_t *nc);
bool nfc_rf_present(nfc_t *nfc);
esp_err_t nfc_i2c_unlock(nfc_t *nfc);  /* clear i2c_locked to allow rf access */

/* fd pin interrupt */
esp_err_t nfc_set_fd_callback(nfc_t *nfc, nfc_fd_cb_t cb, void *arg);
esp_err_t nfc_set_fd_task(nfc_t *nfc, TaskHandle_t task);
bool nfc_wait_fd(nfc_t *nfc, uint32_t timeout_ms);
uint32_t nfc_fd_count(nfc_t *nfc);
bool nfc_fd_pin_level(nfc_t *nfc);

/* fd pin mode configuration */
esp_err_t nfc_set_fd_mode(nfc_t *nfc, nfc_fd_off_t off_mode, nfc_fd_on_t on_mode);
esp_err_t nfc_set_last_ndef_block(nfc_t *nfc, uint8_t block);

/* protection configuration
 * note: auth0 uses nfc page address (i2c block * 4 + page offset within block)
 * e.g. to protect from i2c block 16 onwards: auth0 = 16 * 4 = 64 = 0x40 */
esp_err_t nfc_set_protection(nfc_t *nfc, const nfc_prot_cfg_t *cfg);
esp_err_t nfc_disable_protection(nfc_t *nfc);
esp_err_t nfc_get_protection(nfc_t *nfc, nfc_prot_cfg_t *cfg);

/* helper: convert i2c block to nfc page (first page of block) */
static inline uint8_t nfc_block_to_page(uint8_t block) { return block * 4; }
/* helper: convert nfc page to i2c block */
static inline uint8_t nfc_page_to_block(uint8_t page) { return page / 4; }

/* user data helpers (blocks 1+, leaves block 0 alone for uid/cc) */
esp_err_t nfc_write_bytes(nfc_t *nfc, uint8_t start_block, const uint8_t *data, size_t len);
esp_err_t nfc_read_bytes(nfc_t *nfc, uint8_t start_block, uint8_t *buf, size_t len);
esp_err_t nfc_clear_blocks(nfc_t *nfc, uint8_t start_block, uint8_t count);

#ifdef __cplusplus
}
#endif

#endif