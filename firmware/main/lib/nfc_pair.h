#ifndef NFC_PAIR_H
#define NFC_PAIR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "nfc.h"

typedef enum {
    NFC_PAIR_IDLE,
    NFC_PAIR_NDEF_WRITTEN,
    NFC_PAIR_PHONE_DETECTED,
} nfc_pair_state_t;

typedef void (*nfc_pair_cb_t)(nfc_pair_state_t state, void *arg);

typedef struct {
    nfc_t *nfc;
    uint8_t ble_mac[6];
    uint32_t ndef_timeout_ms;
    nfc_pair_cb_t callback;
    void *cb_arg;
} nfc_pair_config_t;

esp_err_t nfc_pair_init(const nfc_pair_config_t *config);
esp_err_t nfc_pair_write_ndef(void);
esp_err_t nfc_pair_clear_ndef(void);
nfc_pair_state_t nfc_pair_get_state(void);
bool nfc_pair_rf_present(void);
void nfc_pair_deinit(void);

#endif
