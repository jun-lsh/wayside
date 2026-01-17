#ifndef NAME_H
#define NAME_H

#include "nvs.h"
#include "esp_err.h"

#define NAME_MAX_LEN 20  // safe limit for ble advertising

// get or generate a friendly name
// if handle is 0, opens/closes nvs internally using "name" namespace
// if handle is provided, uses that handle (caller manages lifecycle)
// buf must be at least NAME_MAX_LEN bytes
esp_err_t name_get(nvs_handle_t handle, char *buf, size_t buf_len);

#endif