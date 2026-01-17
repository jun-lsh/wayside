#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "keygen.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "keygen";

#define NVS_NAMESPACE   "crypto"
#define NVS_KEY_PUB     "rsa_pub"
#define NVS_KEY_PRIV    "rsa_priv"

static int load_keypair_from_nvs(rsa_key_pair_t *out_keys);
static int save_keypair_to_nvs(const rsa_key_pair_t *keys);

int generate_rsa_keypair(rsa_key_pair_t *out_keys)
{
    int ret = 0;
    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    
    mbedtls_pk_init(&pk);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    const char *pers = "rsa_gen";
    if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     (const unsigned char *)pers, strlen(pers))) != 0) {
        ESP_LOGE(TAG, "mbedtls_ctr_drbg_seed failed: -0x%04x", -ret);
        goto cleanup;
    }

    if ((ret = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA))) != 0) {
        ESP_LOGE(TAG, "mbedtls_pk_setup failed: -0x%04x", -ret);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Generating RSA key...");
    if ((ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), mbedtls_ctr_drbg_random, &ctr_drbg,
                                  KEY_SIZE, EXPONENT)) != 0) {
        ESP_LOGE(TAG, "mbedtls_rsa_gen_key failed: -0x%04x", -ret);
        goto cleanup;
    }

    out_keys->public_key_pem = calloc(1, KEY_BUFFER_SIZE);
    out_keys->private_key_pem = calloc(1, KEY_BUFFER_SIZE);

    if (!out_keys->public_key_pem || !out_keys->private_key_pem) {
        ESP_LOGE(TAG, "Failed to allocate memory for keys");
        ret = -1;
        goto cleanup;
    }

    if ((ret = mbedtls_pk_write_key_pem(&pk, (unsigned char *)out_keys->private_key_pem, KEY_BUFFER_SIZE)) != 0) {
        ESP_LOGE(TAG, "write private key failed: -0x%04x", -ret);
        goto cleanup;
    }

    if ((ret = mbedtls_pk_write_pubkey_pem(&pk, (unsigned char *)out_keys->public_key_pem, KEY_BUFFER_SIZE)) != 0) {
        ESP_LOGE(TAG, "write public key failed: -0x%04x", -ret);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Keys generated and stored in heap");

cleanup:
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    if (ret != 0) {
        if (out_keys->public_key_pem) free(out_keys->public_key_pem);
        if (out_keys->private_key_pem) free(out_keys->private_key_pem);
        out_keys->public_key_pem = NULL;
        out_keys->private_key_pem = NULL;
        return -1;
    }

    return 0;
}

static int load_keypair_from_nvs(rsa_key_pair_t *out_keys)
{
    nvs_handle_t handle;
    esp_err_t err;
    size_t pub_len = 0;
    size_t priv_len = 0;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS namespace not found: %s", esp_err_to_name(err));
        return -1;
    }

    /* Get sizes first */
    err = nvs_get_blob(handle, NVS_KEY_PUB, NULL, &pub_len);
    if (err != ESP_OK || pub_len == 0) {
        ESP_LOGD(TAG, "Public key not found in NVS");
        nvs_close(handle);
        return -1;
    }

    err = nvs_get_blob(handle, NVS_KEY_PRIV, NULL, &priv_len);
    if (err != ESP_OK || priv_len == 0) {
        ESP_LOGD(TAG, "Private key not found in NVS");
        nvs_close(handle);
        return -1;
    }

    /* Allocate and read */
    out_keys->public_key_pem = calloc(1, pub_len);
    out_keys->private_key_pem = calloc(1, priv_len);

    if (!out_keys->public_key_pem || !out_keys->private_key_pem) {
        ESP_LOGE(TAG, "Failed to allocate memory for keys from NVS");
        goto nvs_load_error;
    }

    err = nvs_get_blob(handle, NVS_KEY_PUB, out_keys->public_key_pem, &pub_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read public key: %s", esp_err_to_name(err));
        goto nvs_load_error;
    }

    err = nvs_get_blob(handle, NVS_KEY_PRIV, out_keys->private_key_pem, &priv_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read private key: %s", esp_err_to_name(err));
        goto nvs_load_error;
    }

    nvs_close(handle);
    return 0;

nvs_load_error:
    if (out_keys->public_key_pem) free(out_keys->public_key_pem);
    if (out_keys->private_key_pem) free(out_keys->private_key_pem);
    out_keys->public_key_pem = NULL;
    out_keys->private_key_pem = NULL;
    nvs_close(handle);
    return -1;
}

static int save_keypair_to_nvs(const rsa_key_pair_t *keys)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return -1;
    }

    /* Store public key (including null terminator) */
    size_t pub_len = strlen(keys->public_key_pem) + 1;
    err = nvs_set_blob(handle, NVS_KEY_PUB, keys->public_key_pem, pub_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write public key: %s", esp_err_to_name(err));
        nvs_close(handle);
        return -1;
    }

    /* Store private key (including null terminator) */
    size_t priv_len = strlen(keys->private_key_pem) + 1;
    err = nvs_set_blob(handle, NVS_KEY_PRIV, keys->private_key_pem, priv_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write private key: %s", esp_err_to_name(err));
        nvs_close(handle);
        return -1;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        nvs_close(handle);
        return -1;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Keys saved to NVS");
    return 0;
}

int load_or_generate_keypair(rsa_key_pair_t *out_keys)
{
    if (out_keys == NULL) {
        return -1;
    }

    /* Try to load from NVS first */
    if (load_keypair_from_nvs(out_keys) == 0) {
        ESP_LOGI(TAG, "Loaded keypair from NVS");
        return 0;
    }

    /* No keys in NVS, generate new ones */
    ESP_LOGI(TAG, "No keys in NVS, generating new keypair...");
    if (generate_rsa_keypair(out_keys) != 0) {
        ESP_LOGE(TAG, "Failed to generate keypair");
        return -1;
    }

    /* Save to NVS for next boot */
    if (save_keypair_to_nvs(out_keys) != 0) {
        ESP_LOGW(TAG, "Failed to save keys to NVS (will regenerate on next boot)");
        /* Continue anyway - keys are valid in memory */
    }

    return 0;
}
