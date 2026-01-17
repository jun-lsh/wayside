#ifndef PAIRING_H
#define PAIRING_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define PAIRING_KEY_MAX_LEN         512 
#define PAIRING_BITMASK_MAX_LEN     256
#define KEY_EXCHANGE_URL_MAX_LEN    512

#define PAIRING_PROTOCOL_ID     0x42
#define PAIRING_REBROADCAST_MS  500
#define PAIRING_TIMEOUT_MS      5000
#define PAIRING_HEARTBEAT_MS    1000
#define PAIRING_HEARTBEAT_MISS_MAX 5

typedef enum {
    MSG_HELLO = 1,
    MSG_PROPOSAL,
    MSG_ACCEPT,
    MSG_REJECT,
    MSG_HEARTBEAT,
    MSG_KEY_EXCHANGE,
    MSG_RELAY_URL,
} MSG_TYPE;

typedef enum {
    SEARCHING = 0,
    PROPOSING,
    PAIRED
} BROADCAST_STATE;

/*
 * key exchange (runs automatically after pairing):
 *
 * two badges (A and B) complete pairing, they each hold:
 *   - own public key (my_public_key)
 *   - partner's public key (partner_public_key, received during proposal/accept)
 *
 * The key exchange enables encrypted mugshot url sharing between the user phones:
 *
 *   ┌─────────┐      ┌─────────┐      ┌─────────┐      ┌─────────┐
 *   │ Phone A │      │ Badge A │      │ Badge B │      │ Phone B │
 *   └────┬────┘      └────┬────┘      └────┬────┘      └────┬────┘
 *        │                │                │                │
 *        │     [Badges are now PAIRED via ESP-NOW]          │
 *        │                │                │                │
 *        │                │──KEY_EXCHANGE─>│                │  1. A sends B's pubkey to B (pairing confirm)
 *        │                │<─KEY_EXCHANGE──│                │  2. B sends A's pubkey to A (pairing confirm)
 *        │                │                │                │
 *        │<──PARTNER:B_PK─│                │                │  3. A notifies Phone A of B's pubkey ble
 *        │                │                │──PARTNER:A_PK─>│  4. B notifies Phone B of A's pubkey ble
 *        │                │                │                │
 *        │──ENC_URL:data─>│                │                │  5. Phone A sends encrypted(url, key) for B
 *        │                │                │<─ENC_URL:data──│  6. Phone B sends encrypted(url, key) for A
 *        │                │                │                │
 *        │                │──RELAY_URL────>│                │  7. A unicasts encrypted blob to B
 *        │                │<─RELAY_URL─────│                │  8. B unicasts encrypted blob to A
 *        │                │                │                │
 *        │<─RECV_URL:data─│                │                │  9. A sends received blob to phoneA ble
 *        │                │                │──RECV_URL:data>│ 10. B sends received blob to phoneB ble
 *        │                │                │                │
 *        │   [Phones decrypt locally using their private keys]
 *
 * no crypto happens on the badges.
 * phones handle all cryptographic operations.
 */
typedef struct {
    bool active;
    bool key_sent;
    bool key_confirmed;
    bool notified_phone;
    
    char outgoing_url[KEY_EXCHANGE_URL_MAX_LEN];
    char incoming_url[KEY_EXCHANGE_URL_MAX_LEN];
    bool has_outgoing_url;
    bool outgoing_url_sent;
    bool has_incoming_url;
} key_exchange_ctx_t;

typedef struct __attribute__((packed)) {
    uint8_t protocol_id;       
    uint8_t msg_type;          
    uint8_t sender_mac[6];     
    uint8_t partner_mac[6];    
    uint32_t uptime_ms;        
    uint8_t state;             
    int8_t last_rssi;          
    uint32_t seq_num;          
    uint16_t bitmask_len;
    uint8_t payload[0];
} broadcast_header_t;

typedef struct {
    uint8_t my_mac[6];
    uint8_t partner_mac[6];
    BROADCAST_STATE current_state;
    
    uint32_t last_action_time;
    uint32_t last_heartbeat_sent;
    uint32_t last_heartbeat_recv;
    
    uint32_t heartbeat_seq;
    uint32_t partner_seq;
    int missed_heartbeats;
    int8_t partner_rssi;
    int8_t proposal_rssi;

    uint8_t *bitmask;
    uint16_t bitmask_len;
    
    uint8_t *partner_bitmask;
    uint16_t partner_bitmask_len;

    char my_public_key[PAIRING_KEY_MAX_LEN];
    char partner_public_key[PAIRING_KEY_MAX_LEN];
    
    bool has_bitmask;
    bool has_pubkey;

    uint8_t similarity_threshold;

    key_exchange_ctx_t kex;
} pairing_ctx_t;

esp_err_t pairing_init(pairing_ctx_t *ctx);
void pairing_handle_recv(pairing_ctx_t *ctx, const uint8_t *mac_addr, const uint8_t *data, int len, int8_t rssi);
void pairing_tick(pairing_ctx_t *ctx);
void pairing_reset(pairing_ctx_t *ctx);

void pairing_set_pubkey(pairing_ctx_t *ctx, const char *pub_key);
void pairing_set_bitmask(pairing_ctx_t *ctx, const uint8_t *data, uint16_t len);

bool pairing_is_ready(const pairing_ctx_t *ctx);
bool pairing_get_partner_key(const pairing_ctx_t *ctx, char *out_key, size_t max_len);
bool pairing_get_partner_bitmask(const pairing_ctx_t *ctx, uint8_t *out_data, uint16_t *out_len, uint16_t max_len);

void pairing_set_similarity_threshold(pairing_ctx_t *ctx, uint8_t threshold);

void pairing_set_relay_url(pairing_ctx_t *ctx, const char *url);

#endif // PAIRING_H
