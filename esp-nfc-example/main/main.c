/*
 * main.c - nfc demo with protection and fd modes
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "drivers/nfc.h"
#include "drivers/name.h"
#include "definitions.h"

static const char *TAG = "main";

#define BUILD_TIMESTAMP __DATE__ " " __TIME__

static nfc_t nfc;
static i2c_master_bus_handle_t i2c_bus;
static volatile uint32_t uptime_seconds = 0;

/* memory layout:
 * block 0   - uid, lock bytes, cc (read only / dont touch)
 * block 1   - ndef tlv header
 * block 2-3 - uptime + build info
 * 
 * protection example:
 * block 16+ could be protected (auth0 = 0x40 = page 64 = block 16)
 */
#define BLOCK_INFO  2

/* update nfc tag with current data */
static void update_nfc_data(void)
{
    char buf[64];
    
    /* block 2-3: uptime and build */
    int bytes = snprintf(buf, sizeof(buf), "up:%lus %s", 
             (unsigned long)uptime_seconds, BUILD_TIMESTAMP);
    nfc_write_bytes(&nfc, BLOCK_INFO, (uint8_t *)buf, bytes);
}

/* show current protection config */
static void show_protection(void)
{
    nfc_prot_cfg_t cfg;
    if (nfc_get_protection(&nfc, &cfg) == ESP_OK) {
        ESP_LOGI(TAG, "protection: auth0=0x%02x (page %d, block %d)", 
                 cfg.auth0, cfg.auth0, nfc_page_to_block(cfg.auth0));
        ESP_LOGI(TAG, "  nfc_read_prot=%d, authlim=%d", cfg.nfc_read_prot, cfg.authlim);
        ESP_LOGI(TAG, "  i2c_prot=%d, sram_prot=%d", cfg.i2c_prot, cfg.sram_prot);
    }
}

/* nfc task - updates data and handles phone scans */
static void nfc_task(void *arg)
{
    ESP_LOGI(TAG, "nfc task running");
    
    nfc_set_fd_task(&nfc, xTaskGetCurrentTaskHandle());
    
    /* configure fd pin:
     * - goes low when rf field on
     * - goes high when last ndef block read (block 3 in this example) */
    nfc_set_fd_mode(&nfc, NFC_FD_OFF_LAST_NDEF, NFC_FD_ON_RF_ON);
    nfc_set_last_ndef_block(&nfc, 3);  /* fd goes high after block 3 is read */
    ESP_LOGI(TAG, "fd mode: low on rf, high after block 3 read");
    
    /* show current protection settings */
    show_protection();
    
    /* example: optionally enable protection on blocks 16+
     * uncomment to test password protection */
    /*
    nfc_prot_cfg_t prot = {
        .auth0 = nfc_block_to_page(16),  // protect from block 16 onwards
        .nfc_read_prot = false,          // write protect only (read still works)
        .authlim = 0,                    // unlimited auth attempts
        .i2c_prot = NFC_I2C_PROT_NONE,   // i2c has full access
        .sram_prot = false,
        .pwd = {0x01, 0x02, 0x03, 0x04},
        .pack = {0xAB, 0xCD},
    };
    nfc_set_protection(&nfc, &prot);
    show_protection();
    */
    
    bool was_rf = false;
    uint32_t last_update = 0;
    
    /* write initial data */
    update_nfc_data();
    ESP_LOGI(TAG, "wrote initial nfc data");
    
    while (1) {
        /* 100ms poll for responsive updates */
        nfc_wait_fd(&nfc, 100);
        
        /* track uptime */
        uint32_t now = xTaskGetTickCount() / configTICK_RATE_HZ;
        if (now != last_update) {
            uptime_seconds = now;
            last_update = now;
        }
        
        bool rf = nfc_rf_present(&nfc);
        
        if (rf && !was_rf) {
            /* phone arrived */
            ESP_LOGI(TAG, "** phone detected **");
        }
        else if (!rf && was_rf) {
            /* phone left - wait a bit for chip to settle */
            vTaskDelay(pdMS_TO_TICKS(10));
            
            ESP_LOGI(TAG, "phone left, reading back data...");
            
            /* read what's on the tag now */
            uint8_t buf[64];
            if (nfc_read_bytes(&nfc, BLOCK_INFO, buf, sizeof(buf)) == ESP_OK) {
                if (buf[63] != 0) {
                    buf[63] = 0;  /* ensure null termination */
                }
                /* print as string and hex */
                ESP_LOGI(TAG, "  str: %s", buf);
                char hex_str[3 * 64 + 1] = {0};
                char *p = hex_str;
                for (int i = 0; i < 64; i++) {
                    p += sprintf(p, "%02x ", buf[i]);
                }
                ESP_LOGI(TAG, "  hex: %s", hex_str);
            }
            
            /* update for next scan */
            update_nfc_data();
            ESP_LOGI(TAG, "updated nfc data (up:%lus)", (unsigned long)uptime_seconds);
        }
        else if (!rf) {
            /* no phone - update every 2 seconds */
            static uint32_t last_nfc_update = 0;
            if (uptime_seconds - last_nfc_update >= 2) {
                update_nfc_data();
                last_nfc_update = uptime_seconds;
            }
        }
        
        was_rf = rf;
    }
}

void app_main(void)
{
    esp_err_t ret;
    
    ESP_LOGI(TAG, "build: %s", BUILD_TIMESTAMP);
    
    /* power on nfc chip */
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << NFC_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(NFC_PWR_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "nfc power on");
    
    /* init nvs */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }

    char device_name[32];
    nvs_handle_t my_handle;
    nvs_open("name", NVS_READWRITE, &my_handle);
    name_get(my_handle, device_name, sizeof(device_name));
    nvs_close(my_handle);

    ESP_LOGI(TAG, "device name: %s", device_name);
    
    /* init i2c bus */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = NFC_SDA_PIN,
        .scl_io_num = NFC_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    
    ret = i2c_new_master_bus(&bus_cfg, &i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c init failed");
        return;
    }
    
    /* init nfc */
    ret = nfc_init(&nfc, i2c_bus, NFC_I2C_ADDR, NFC_I2C_FREQ_HZ, NFC_FD_PIN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nfc init failed");
        return;
    }
    
    /* show uid */
    uint8_t block0[16];
    if (nfc_read_block(&nfc, 0x00, block0, true) == ESP_OK) {
        ESP_LOGI(TAG, "uid: %02x:%02x:%02x:%02x:%02x:%02x:%02x",
                 block0[0], block0[1], block0[2], block0[3],
                 block0[4], block0[5], block0[6]);
    }
    
    /* start task */
    xTaskCreate(nfc_task, "nfc", 4096, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "ready - scan with phone!");
}