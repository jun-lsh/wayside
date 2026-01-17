#ifndef PAIRING_H
#define PAIRING_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// --- CONFIGURATION ---
// Max size for a 1024-bit RSA key in PEM format is approx ~250-300 bytes.
// We allocate 512 bytes to be safe and compatible with future key sizes.
// ESP-NOW v2 supports up to 1470 bytes, so this fits easily.
#define PAIRING_KEY_MAX_LEN     512 

// Protocol Constants (Same as original)
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
    MSG_HEARTBEAT
} MSG_TYPE;

typedef enum {
    SEARCHING = 0,
    PROPOSING,
    PAIRED
} BROADCAST_STATE;

// --- PACKET STRUCTURE ---
// We use a packed struct. For HELLO/HEARTBEAT we only send the header.
// For PROPOSAL/ACCEPT we send the header + public_key.
typedef struct __attribute__((packed)) {
    // Header (Always sent)
    uint8_t protocol_id;       
    uint8_t msg_type;          
    uint8_t sender_mac[6];     
    uint8_t partner_mac[6];    
    uint32_t uptime_ms;        
    uint8_t state;             
    int8_t last_rssi;          
    uint32_t seq_num;          
    uint32_t bitmask;
    
    // Payload (Sent only for PROPOSAL/ACCEPT)
    char public_key[PAIRING_KEY_MAX_LEN]; 
} broadcast_t;

// --- CONTEXT ---
typedef struct {
    uint8_t my_mac[6];
    uint8_t partner_mac[6];
    BROADCAST_STATE current_state;
    
    // Timing
    uint32_t last_action_time;
    uint32_t last_heartbeat_sent;
    uint32_t last_heartbeat_recv;
    
    // Stats
    uint32_t heartbeat_seq;
    uint32_t partner_seq;
    int missed_heartbeats;
    int8_t partner_rssi;
    uint32_t bitmask;

    // --- NEW: Key Management ---
    char my_public_key[PAIRING_KEY_MAX_LEN];      // Key received from BLE
    char partner_public_key[PAIRING_KEY_MAX_LEN]; // Key received from Peer
    bool is_broadcasting;                         // Gatekeeper: Don't start until key is set

} pairing_ctx_t;

// --- PUBLIC API ---
esp_err_t pairing_init(pairing_ctx_t *ctx);
void pairing_handle_recv(pairing_ctx_t *ctx, const uint8_t *mac_addr, const uint8_t *data, int len, int8_t rssi);
void pairing_tick(pairing_ctx_t *ctx);
void pairing_reset(pairing_ctx_t *ctx);

// Set the key received via BLE and enable the state machine
void pairing_set_config(pairing_ctx_t *ctx, const char *pub_key);

// Accessor to get the partner's key after pairing
bool pairing_get_partner_key(const pairing_ctx_t *ctx, char *out_key, size_t max_len);

#endif // PAIRING_H