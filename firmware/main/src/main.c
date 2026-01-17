#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "wifi_task.h"
#include "espnow.h"
#include "keygen.h"
#include "ble_task.h"
#include "nfc.h"
#include "definitions.h"
#include "buzzer.h"
#include "hnr26_badge.h"
#include "proximity.h"

static const char *TAG = "main";

static nfc_handle_t nfc_handle;
static i2c_master_bus_handle_t i2c_bus = NULL;
static i2c_master_dev_handle_t nfc_i2c_dev = NULL;
static bool nfc_available = false;

static void IRAM_ATTR nfc_fd_isr_callback(void *arg)
{
}

static void nfc_log_status(void)
{
    esp_err_t ret;
    nfc_session_regs_t regs;
    
    ESP_LOGI(TAG, "=== NFC Status ===");
    ESP_LOGI(TAG, "FD interrupt count: %lu", (unsigned long)nfc_get_fd_int_count(&nfc_handle));
    ESP_LOGI(TAG, "FD pin state: %s", nfc_read_fd_pin(&nfc_handle) ? "HIGH" : "LOW");
    
    ret = nfc_read_session_registers(&nfc_handle, &regs);
    if (ret == ESP_OK) {
        /* NC_REG bits */
        ESP_LOGI(TAG, "NC_REG: 0x%02X", regs.nc_reg);
        ESP_LOGI(TAG, "  - I2C_RST/NFC_DIS: %s", (regs.nc_reg & NFC_NC_NFCS_I2C_RST) ? "yes" : "no");
        ESP_LOGI(TAG, "  - Pass-through: %s", (regs.nc_reg & NFC_NC_PTHRU_ON_OFF) ? "on" : "off");
        ESP_LOGI(TAG, "  - SRAM mirror: %s", (regs.nc_reg & NFC_NC_SRAM_MIRROR) ? "on" : "off");
        ESP_LOGI(TAG, "  - Transfer dir: %s", (regs.nc_reg & NFC_NC_TRANSFER_DIR) ? "I2C->RF" : "RF->I2C");
        
        /* NS_REG bits */
        ESP_LOGI(TAG, "NS_REG: 0x%02X", regs.ns_reg);
        ESP_LOGI(TAG, "  - RF field: %s", (regs.ns_reg & NFC_NS_RF_FIELD_PRESENT) ? "PRESENT" : "not detected");
        ESP_LOGI(TAG, "  - NDEF data read: %s", (regs.ns_reg & NFC_NS_NDEF_DATA_READ) ? "yes" : "no");
        ESP_LOGI(TAG, "  - I2C locked: %s", (regs.ns_reg & NFC_NS_I2C_LOCKED) ? "yes" : "no");
        ESP_LOGI(TAG, "  - RF locked: %s", (regs.ns_reg & NFC_NS_RF_LOCKED) ? "yes" : "no");
        ESP_LOGI(TAG, "  - SRAM I2C ready: %s", (regs.ns_reg & NFC_NS_SRAM_I2C_READY) ? "yes" : "no");
        ESP_LOGI(TAG, "  - SRAM RF ready: %s", (regs.ns_reg & NFC_NS_SRAM_RF_READY) ? "yes" : "no");
        ESP_LOGI(TAG, "  - EEPROM write err: %s", (regs.ns_reg & NFC_NS_EEPROM_WR_ERR) ? "ERROR" : "ok");
        ESP_LOGI(TAG, "  - EEPROM write busy: %s", (regs.ns_reg & NFC_NS_EEPROM_WR_BUSY) ? "busy" : "idle");
        
        ESP_LOGI(TAG, "Last NDEF block: 0x%02X", regs.last_ndef_block);
        ESP_LOGI(TAG, "SRAM mirror block: 0x%02X", regs.sram_mirror_block);
        ESP_LOGI(TAG, "Watchdog: 0x%02X%02X", regs.wdt_ms, regs.wdt_ls);
        ESP_LOGI(TAG, "I2C clock stretch: 0x%02X", regs.i2c_clock_str);
    } else {
        ESP_LOGE(TAG, "Failed to read session registers: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "==================");
}

static esp_err_t nfc_gpio_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << NFC_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure NFC power pin: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = gpio_set_level(NFC_PWR_PIN, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set NFC power pin high: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "NFC power pin (GPIO%d) set HIGH", NFC_PWR_PIN);
    return ESP_OK;
}

static void nfc_task(void *pvParameters)
{
    ESP_LOGI(TAG, "NFC task started");
    
    nfc_set_fd_notify_task(&nfc_handle, xTaskGetCurrentTaskHandle());
    
    while (1) {
        bool got_interrupt = nfc_wait_fd_interrupt(&nfc_handle, 5000);
        
        if (got_interrupt) {
            nfc_log_status();
        }
    }
}

static void nfc_init_if_connected(void)
{
    esp_err_t ret;
    
    ret = nfc_gpio_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NFC GPIO init failed, skipping NFC");
        return;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
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
        ESP_LOGW(TAG, "I2C bus init failed: %s, skipping NFC", esp_err_to_name(ret));
        return;
    }
    
    ret = i2c_master_probe(i2c_bus, NFC_I2C_ADDR_DEFAULT, NFC_I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NFC not detected on I2C bus (addr 0x%02X), skipping NFC", NFC_I2C_ADDR_DEFAULT);
        i2c_del_master_bus(i2c_bus);
        i2c_bus = NULL;
        gpio_set_level(NFC_PWR_PIN, 0);  /* Power down */
        return;
    }
    
    ESP_LOGI(TAG, "NFC device detected at 0x%02X", NFC_I2C_ADDR_DEFAULT);
    
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = NFC_I2C_ADDR_DEFAULT,
        .scl_speed_hz = NFC_I2C_FREQ_HZ,
    };
    
    ret = i2c_master_bus_add_device(i2c_bus, &dev_config, &nfc_i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(i2c_bus);
        i2c_bus = NULL;
        return;
    }
    
    ESP_LOGI(TAG, "I2C master initialized (SDA: GPIO%d, SCL: GPIO%d, %lu Hz)", 
             NFC_SDA_PIN, NFC_SCL_PIN, (unsigned long)NFC_I2C_FREQ_HZ);
    
    ret = nfc_init(&nfc_handle, nfc_i2c_dev, NFC_FD_PIN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NFC driver init failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(nfc_i2c_dev);
        i2c_del_master_bus(i2c_bus);
        i2c_bus = NULL;
        return;
    }
    
    nfc_set_fd_callback(&nfc_handle, nfc_fd_isr_callback, NULL);
    
    nfc_session_regs_t session_regs;
    ret = nfc_read_session_registers(&nfc_handle, &session_regs);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NFC session registers:");
        ESP_LOGI(TAG, "  NC_REG: 0x%02X", session_regs.nc_reg);
        ESP_LOGI(TAG, "  NS_REG: 0x%02X (RF field: %s)", 
                 session_regs.ns_reg,
                 (session_regs.ns_reg & NFC_NS_RF_FIELD_PRESENT) ? "present" : "not detected");
    } else {
        ESP_LOGW(TAG, "Failed to read NFC session registers: %s", esp_err_to_name(ret));
    }
    
    nfc_available = true;
    
    xTaskCreatePinnedToCore(nfc_task, "nfc_task", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "NFC initialized and task started");
}

void app_main(void)
{
    // temp
    gpio_config_t gpio0_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t ret = gpio_config(&gpio0_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO 0: %s", esp_err_to_name(ret));
        return;
    }
    ret = gpio_set_level(GPIO_NUM_0, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set GPIO 0 high: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // rsa_key_pair_t *global_keys = malloc(sizeof(rsa_key_pair_t));
    // if (global_keys == NULL) {
    //     ESP_LOGE(TAG, "Failed to allocate memory for keys");
    //     return;
    // }

    // if (load_or_generate_keypair(global_keys) != 0) {
    //     ESP_LOGE(TAG, "Key loading/generation failed, cannot start tasks.");
    //     free(global_keys);
    //     return;
    // }

    
    buzzer_config_t config = {
        .gpio_num = 3,
        .frequency = 2700,
        .initial_volume = 100
    };
    ret = buzzer_init(&config);

    ret = hnr26_badge_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Badge init failed: %s (LEDs may not work)", esp_err_to_name(ret));
    }

    ret = proximity_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Proximity init failed: %s", esp_err_to_name(ret));
    }

    // nfc_init_if_connected();
    wifi_init();
    espnow_init();
    ble_init();
}
