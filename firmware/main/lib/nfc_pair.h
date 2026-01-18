#ifndef NFC_PAIR_H
#define NFC_PAIR_H

#include "esp_err.h"
#include "nfc.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// nfc pairing state
typedef enum {
    NFC_PAIR_IDLE,
    NFC_PAIR_ADVERTISING,
    NFC_PAIR_NDEF_WRITTEN,
    NFC_PAIR_PAIRED,
} nfc_pair_state_t;

// callback for state changes
typedef void (*nfc_pair_cb_t)(nfc_pair_state_t state, void *arg);

// configuration for nfc pairing
typedef struct {
    nfc_t *nfc;                    // nfc handle (must be initialized)
    uint8_t ble_mac[6];            // ble mac address
    const char *device_name;       // device name for ndef
    uint32_t ndef_timeout_ms;      // how long to expose ndef before clearing (0=forever)
    nfc_pair_cb_t callback;        // optional state change callback
    void *cb_arg;                  // callback argument
} nfc_pair_config_t;

// init nfc pairing module
esp_err_t nfc_pair_init(const nfc_pair_config_t *config);

// write ble pairing ndef to tag (call after ble advertising starts)
esp_err_t nfc_pair_write_ndef(void);

// clear ndef and write default message
esp_err_t nfc_pair_clear_ndef(void);

// get current state
nfc_pair_state_t nfc_pair_get_state(void);

// check if rf field detected (phone present)
bool nfc_pair_rf_present(void);

// deinit
void nfc_pair_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
