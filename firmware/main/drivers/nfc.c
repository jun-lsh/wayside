/*
 * nfc.c - nt3h2111 nfc i2c driver
 */

#include "nfc.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "nfc";

/* forward declaration */
esp_err_t nfc_i2c_unlock(nfc_t *nfc);

/* fd pin isr */
static void IRAM_ATTR fd_isr(void *arg)
{
    nfc_t *nfc = (nfc_t *)arg;
    BaseType_t woken = pdFALSE;
    
    nfc->fd_count++;
    
    if (nfc->fd_cb) {
        nfc->fd_cb(nfc->fd_cb_arg);
    }
    
    if (nfc->notify_task) {
        vTaskNotifyGiveFromISR(nfc->notify_task, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

/* configure fd pin with interrupt */
static esp_err_t fd_gpio_init(nfc_t *nfc, gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE  /* trigger on both rising and falling */
    };
    
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) return ret;
    
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    
    ret = gpio_isr_handler_add(pin, fd_isr, nfc);
    if (ret != ESP_OK) return ret;
    
    nfc->fd_pin = pin;
    return ESP_OK;
}

esp_err_t nfc_init(nfc_t *nfc, i2c_master_bus_handle_t bus, uint8_t addr, uint32_t freq_hz, gpio_num_t fd_pin)
{
    if (!nfc || !bus) return ESP_ERR_INVALID_ARG;
    
    memset(nfc, 0, sizeof(nfc_t));
    
    /* add device to bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = freq_hz,
    };
    
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &nfc->dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* setup fd pin interrupt */
    if (fd_pin != GPIO_NUM_NC) {
        ret = fd_gpio_init(nfc, fd_pin);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "fd pin init failed: %s", esp_err_to_name(ret));
        }
    }
    
    ESP_LOGI(TAG, "init ok (addr=0x%02x, fd=gpio%d)", addr, fd_pin);
    return ESP_OK;
}

esp_err_t nfc_deinit(nfc_t *nfc)
{
    if (!nfc || !nfc->dev) return ESP_ERR_INVALID_STATE;
    
    if (nfc->fd_pin != GPIO_NUM_NC) {
        gpio_isr_handler_remove(nfc->fd_pin);
    }
    
    return i2c_master_bus_rm_device(nfc->dev);
}

esp_err_t nfc_read_block(nfc_t *nfc, uint8_t block, uint8_t *data, bool release_lock)
{
    if (!nfc || !nfc->dev || !data) return ESP_ERR_INVALID_ARG;
    
    /* write block address, then read 16 bytes */
    esp_err_t ret = i2c_master_transmit(nfc->dev, &block, 1, NFC_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;
    
    ret = i2c_master_receive(nfc->dev, data, NFC_BLOCK_SIZE, NFC_I2C_TIMEOUT_MS);
    
    if (release_lock) {
        /* release i2c lock so rf can access */
        nfc_i2c_unlock(nfc);
    }
    
    return ret;
}

esp_err_t nfc_write_block(nfc_t *nfc, uint8_t block, const uint8_t *data, bool release_lock)
{
    if (!nfc || !nfc->dev || !data) return ESP_ERR_INVALID_ARG;
    
    /* write block address + 16 bytes data */
    uint8_t buf[1 + NFC_BLOCK_SIZE];
    buf[0] = block;
    memcpy(&buf[1], data, NFC_BLOCK_SIZE);
    
    esp_err_t ret = i2c_master_transmit(nfc->dev, buf, sizeof(buf), NFC_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;
    
    /* wait for eeprom write (not needed for sram) by polling EEPROM_WR_BUSY */
    if (block < NFC_SRAM_START) {
        while(1) {
            uint8_t ns = 0;
            ret = nfc_get_ns_reg(nfc, &ns);
            if (ret != ESP_OK) return ret;
            if ((ns & NFC_NS_EEPROM_BUSY) == 0) break;
            vTaskDelay(pdMS_TO_TICKS(NFC_EEPROM_WRITE_DELAY_MS));
        }
    }
    
    if (release_lock) {
        /* release i2c lock so rf can access */
        nfc_i2c_unlock(nfc);
    }
    
    return ESP_OK;
}

esp_err_t nfc_read_reg(nfc_t *nfc, uint8_t reg, uint8_t *val)
{
    if (!nfc || !nfc->dev || !val) return ESP_ERR_INVALID_ARG;
    
    /* write session block + reg offset, then read 1 byte */
    uint8_t cmd[2] = { NFC_SESSION_REG_BLOCK, reg };
    
    esp_err_t ret = i2c_master_transmit(nfc->dev, cmd, 2, NFC_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;
    
    ret = i2c_master_receive(nfc->dev, val, 1, NFC_I2C_TIMEOUT_MS);
    
    /* release i2c lock so rf can access (skip if reading ns_reg to avoid recursion) */
    if (reg != NFC_REG_NS) {
        nfc_i2c_unlock(nfc);
    }
    
    return ret;
}

esp_err_t nfc_write_reg(nfc_t *nfc, uint8_t reg, uint8_t mask, uint8_t val)
{
    if (!nfc || !nfc->dev) return ESP_ERR_INVALID_ARG;
    
    /* write: session block, reg, mask, value */
    uint8_t cmd[4] = { NFC_SESSION_REG_BLOCK, reg, mask, val };
    
    esp_err_t ret = i2c_master_transmit(nfc->dev, cmd, 4, NFC_I2C_TIMEOUT_MS);
    
    /* release i2c lock so rf can access (skip if writing ns_reg to avoid recursion) */
    if (ret == ESP_OK && reg != NFC_REG_NS) {
        nfc_i2c_unlock(nfc);
    }
    
    return ret;
}

esp_err_t nfc_get_ns_reg(nfc_t *nfc, uint8_t *ns)
{
    esp_err_t ret = nfc_read_reg(nfc, NFC_REG_NS, ns);
    /* explicitly unlock since read_reg skips unlock for ns_reg */
    if (ret == ESP_OK) {
        nfc_i2c_unlock(nfc);
    }
    return ret;
}

esp_err_t nfc_get_nc_reg(nfc_t *nfc, uint8_t *nc)
{
    return nfc_read_reg(nfc, NFC_REG_NC, nc);
}

bool nfc_rf_present(nfc_t *nfc)
{
    uint8_t ns = 0;
    if (nfc_get_ns_reg(nfc, &ns) != ESP_OK) return false;
    return (ns & NFC_NS_RF_FIELD) != 0;
}

esp_err_t nfc_i2c_unlock(nfc_t *nfc)
{
    /* clear i2c_locked bit (bit 6) in ns_reg to release lock for rf access */
    return nfc_write_reg(nfc, NFC_REG_NS, NFC_NS_I2C_LOCKED, 0x00);
}

esp_err_t nfc_set_fd_callback(nfc_t *nfc, nfc_fd_cb_t cb, void *arg)
{
    if (!nfc) return ESP_ERR_INVALID_ARG;
    nfc->fd_cb = cb;
    nfc->fd_cb_arg = arg;
    return ESP_OK;
}

esp_err_t nfc_set_fd_task(nfc_t *nfc, TaskHandle_t task)
{
    if (!nfc) return ESP_ERR_INVALID_ARG;
    nfc->notify_task = task;
    return ESP_OK;
}

bool nfc_wait_fd(nfc_t *nfc, uint32_t timeout_ms)
{
    if (!nfc || !nfc->notify_task) return false;
    
    TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return ulTaskNotifyTake(pdTRUE, ticks) > 0;
}

uint32_t nfc_fd_count(nfc_t *nfc)
{
    return nfc ? nfc->fd_count : 0;
}

bool nfc_fd_pin_level(nfc_t *nfc)
{
    if (!nfc || nfc->fd_pin == GPIO_NUM_NC) return false;
    return gpio_get_level(nfc->fd_pin) != 0;
}

esp_err_t nfc_set_fd_mode(nfc_t *nfc, nfc_fd_off_t off_mode, nfc_fd_on_t on_mode)
{
    if (!nfc) return ESP_ERR_INVALID_ARG;
    
    uint8_t mask = NFC_NC_FD_OFF_MASK | NFC_NC_FD_ON_MASK;
    uint8_t val = ((off_mode & 0x03) << NFC_NC_FD_OFF_SHIFT) |
                  ((on_mode & 0x03) << NFC_NC_FD_ON_SHIFT);
    
    return nfc_write_reg(nfc, NFC_REG_NC, mask, val);
}

esp_err_t nfc_set_last_ndef_block(nfc_t *nfc, uint8_t block)
{
    if (!nfc) return ESP_ERR_INVALID_ARG;
    
    /* last_ndef_block is session reg offset 1, full byte write */
    return nfc_write_reg(nfc, NFC_REG_LAST_NDEF, 0xFF, block);
}

esp_err_t nfc_set_protection(nfc_t *nfc, const nfc_prot_cfg_t *cfg)
{
    if (!nfc || !cfg) return ESP_ERR_INVALID_ARG;
    
    esp_err_t ret;
    uint8_t block[NFC_BLOCK_SIZE];
    
    /* read block 0x38 to preserve existing data, then set auth0 */
    ret = nfc_read_block(nfc, NFC_AUTH_BLOCK, block, false);
    if (ret != ESP_OK) return ret;
    
    block[15] = cfg->auth0;  /* auth0 at byte 15 */
    
    ret = nfc_write_block(nfc, NFC_AUTH_BLOCK, block, false);
    if (ret != ESP_OK) return ret;
    
    /* read block 0x39 to preserve rfu bytes */
    ret = nfc_read_block(nfc, NFC_ACCESS_BLOCK, block, false);
    if (ret != ESP_OK) return ret;
    
    /* access byte: nfc_prot (bit 7), authlim (bits 2-0) */
    block[NFC_ACCESS_BYTE] = (cfg->nfc_read_prot ? NFC_ACCESS_NFC_PROT : 0) |
                             (cfg->authlim & NFC_ACCESS_AUTHLIM_MASK);
    
    /* password (4 bytes) */
    memcpy(&block[NFC_PWD_OFFSET], cfg->pwd, 4);
    
    /* pack (2 bytes) */
    memcpy(&block[NFC_PACK_OFFSET], cfg->pack, 2);
    
    /* pt_i2c: sram_prot (bit 2), i2c_prot (bits 1-0) */
    block[NFC_PT_I2C_OFFSET] = (cfg->sram_prot ? NFC_PT_SRAM_PROT : 0) |
                               (cfg->i2c_prot & NFC_PT_I2C_PROT_MASK);
    
    ret = nfc_write_block(nfc, NFC_ACCESS_BLOCK, block, true);
    if (ret != ESP_OK) return ret;
    
    ESP_LOGI(TAG, "protection set: auth0=0x%02x, i2c_prot=%d", cfg->auth0, cfg->i2c_prot);
    return ESP_OK;
}

esp_err_t nfc_disable_protection(nfc_t *nfc)
{
    /* set auth0 to 0xff to disable protection */
    nfc_prot_cfg_t cfg = {
        .auth0 = 0xFF,
        .nfc_read_prot = false,
        .authlim = 0,
        .i2c_prot = NFC_I2C_PROT_NONE,
        .sram_prot = false,
        .pwd = {0xFF, 0xFF, 0xFF, 0xFF},
        .pack = {0x00, 0x00},
    };
    return nfc_set_protection(nfc, &cfg);
}

esp_err_t nfc_get_protection(nfc_t *nfc, nfc_prot_cfg_t *cfg)
{
    if (!nfc || !cfg) return ESP_ERR_INVALID_ARG;
    
    esp_err_t ret;
    uint8_t block[NFC_BLOCK_SIZE];
    
    /* read block 0x38 for auth0 */
    ret = nfc_read_block(nfc, NFC_AUTH_BLOCK, block, false);
    if (ret != ESP_OK) return ret;
    
    cfg->auth0 = block[15];
    
    /* read block 0x39 for access, pt_i2c (pwd/pack always read as 0) */
    ret = nfc_read_block(nfc, NFC_ACCESS_BLOCK, block, true);
    if (ret != ESP_OK) return ret;
    
    cfg->nfc_read_prot = (block[NFC_ACCESS_BYTE] & NFC_ACCESS_NFC_PROT) != 0;
    cfg->authlim = block[NFC_ACCESS_BYTE] & NFC_ACCESS_AUTHLIM_MASK;
    cfg->i2c_prot = block[NFC_PT_I2C_OFFSET] & NFC_PT_I2C_PROT_MASK;
    cfg->sram_prot = (block[NFC_PT_I2C_OFFSET] & NFC_PT_SRAM_PROT) != 0;
    
    /* pwd and pack always read as 0x00 from chip */
    memset(cfg->pwd, 0, 4);
    memset(cfg->pack, 0, 2);
    
    return ESP_OK;
}

esp_err_t nfc_write_bytes(nfc_t *nfc, uint8_t start_block, const uint8_t *text, size_t len)
{
    if (!nfc || !text || start_block < 1) return ESP_ERR_INVALID_ARG;
    
    /* write bytes across multiple blocks if needed */
    size_t offset = 0;
    uint8_t block = start_block;
    uint8_t buf[NFC_BLOCK_SIZE];
    
    while (offset < len && block < NFC_SRAM_START) {
        bool is_last = (offset + NFC_BLOCK_SIZE >= len);
        size_t chunk = is_last ? (len - offset) : NFC_BLOCK_SIZE;
        memset(buf, 0, NFC_BLOCK_SIZE);
        memcpy(buf, text + offset, chunk);
        
        esp_err_t ret = nfc_write_block(nfc, block, buf, is_last);
        if (ret != ESP_OK) return ret;
        
        offset += chunk;
        block++;
    }
    
    return ESP_OK;
}

esp_err_t nfc_read_bytes(nfc_t *nfc, uint8_t start_block, uint8_t *buf, size_t len)
{
    if (!nfc || !buf || start_block < 1 || len == 0) return ESP_ERR_INVALID_ARG;
    
    /* read bytes across multiple blocks if necessary */
    size_t total = 0;
    uint8_t block = start_block;
    uint8_t tmp[NFC_BLOCK_SIZE];
    
    while (total < len && block < NFC_SRAM_START) {
        bool is_last = (total + NFC_BLOCK_SIZE >= len);
        size_t chunk = is_last ? (len - total) : NFC_BLOCK_SIZE;
        
        esp_err_t ret = nfc_read_block(nfc, block, tmp, is_last);
        if (ret != ESP_OK) return ret;
        
        memcpy(buf + total, tmp, chunk);
        total += chunk;
        block++;
    }

    return ESP_OK;
}

esp_err_t nfc_clear_blocks(nfc_t *nfc, uint8_t start_block, uint8_t count)
{
    if (!nfc || start_block < 1) return ESP_ERR_INVALID_ARG;
    
    uint8_t zeros[NFC_BLOCK_SIZE] = {0};
    
    for (uint8_t i = 0; i < count && (start_block + i) < NFC_SRAM_START; i++) {
        bool is_last = (i == count - 1);
        esp_err_t ret = nfc_write_block(nfc, start_block + i, zeros, is_last);
        if (ret != ESP_OK) return ret;
    }
    
    return ESP_OK;
}