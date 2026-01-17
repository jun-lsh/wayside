#include "name.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

#define NVS_NAMESPACE "name"
#define NVS_KEY       "friendly"

// short words for name generation (3-6 chars each)
static const char *word1[] = {
    "red", "blue", "fast", "cool", "tiny", "bold",
    "warm", "dark", "wild", "calm", "soft", "keen"
};

static const char *word2[] = {
    "fox", "owl", "bee", "cat", "wolf", "hawk",
    "bear", "lynx", "crow", "hare", "moth", "seal"
};

#define WORD1_COUNT  (sizeof(word1) / sizeof(word1[0]))
#define WORD2_COUNT (sizeof(word2) / sizeof(word2[0]))

// generate a random name 
static void generate_name(char *buf, size_t buf_len)
{
    uint32_t r = esp_random();
    const char *w1 = word1[r % WORD1_COUNT];
    const char *w2 = word2[(r >> 8) % WORD2_COUNT];
    uint8_t num = (r >> 16) % 100;

    // combine into buffer
    snprintf(buf, buf_len, "%s%s%02d", w1, w2, num);
}

esp_err_t name_get(nvs_handle_t handle, char *buf, size_t buf_len)
{
    if (!buf || buf_len < NAME_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    nvs_handle_t h = handle;
    bool own_handle = (handle == 0);

    // open nvs if no handle provided
    if (own_handle) {
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
        if (err != ESP_OK) {
            return err;
        }
    }

    // try to read existing name
    size_t len = buf_len;
    err = nvs_get_str(h, NVS_KEY, buf, &len);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // no name stored, generate one
        generate_name(buf, buf_len);

        err = nvs_set_str(h, NVS_KEY, buf);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    }

    if (own_handle) {
        nvs_close(h);
    }

    return err;
}