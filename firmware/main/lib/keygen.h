#ifndef KEYGEN_H
#define KEYGEN_H

typedef struct {
    char *public_key_pem;
    char *private_key_pem;
} rsa_key_pair_t;

#define KEY_SIZE 2048
#define EXPONENT 65537
#define KEY_BUFFER_SIZE 4096

/**
 * @brief Generate a new RSA keypair
 * 
 * @param out_keys Pointer to key pair structure to populate
 * @return 0 on success, -1 on failure
 */
int generate_rsa_keypair(rsa_key_pair_t *out_keys);

/**
 * @brief Load keypair from NVS, or generate and save if not found
 * 
 * This function first attempts to load an existing keypair from NVS.
 * If no keypair exists, it generates a new one and saves it to NVS.
 * To force regeneration, erase the NVS partition before calling.
 * 
 * @param out_keys Pointer to key pair structure to populate
 * @return 0 on success, -1 on failure
 */
int load_or_generate_keypair(rsa_key_pair_t *out_keys);

#endif /* KEYGEN_H */
