#ifndef NFC_PAIR_H
#define NFC_PAIR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "nfc.h"

typedef enum {
    NFC_PAIR_IDLE,
    NFC_PAIR_READY,
    NFC_PAIR_PHONE_DETECTED,
    NFC_PAIR_ADVERTISING,
    NFC_PAIR_CONNECTED,
    NFC_PAIR_AUTHENTICATED,
} nfc_pair_state_t;

typedef void (*nfc_pair_cb_t)(nfc_pair_state_t state, void *arg);

typedef struct {
    nfc_t *nfc;
    const char *device_name;
    uint8_t ble_mac[6];
    uint32_t otp_refresh_ms;
    uint32_t adv_timeout_sec;
    nfc_pair_cb_t callback;
    void *cb_arg;
} nfc_pair_config_t;

esp_err_t nfc_pair_init(const nfc_pair_config_t *config);
esp_err_t nfc_pair_write_ndef(void);
uint32_t nfc_pair_get_otp(void);
void nfc_pair_get_otp_str(char *buf, size_t buf_len);
nfc_pair_state_t nfc_pair_get_state(void);
bool nfc_pair_rf_present(void);
esp_err_t nfc_pair_start_advertising(void);
esp_err_t nfc_pair_stop_advertising(void);
esp_err_t nfc_pair_clear_ndef(void);
void nfc_pair_deinit(void);

#endif
