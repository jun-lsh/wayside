// data needed:
// public key generated from mac address 
// bitmask fetched from stuff
// max length -> either malloced or just 16 bits 

#ifndef STATES_H
#define STATES_H

#include <stdint.h>
#include "esp_now.h"

#define PAIRING_PROTOCOL_ID 0xAA

typedef enum {
    SEARCHING,   /* Looking for peers, broadcasting HELLO */
    PROPOSING,   /* Sent a proposal, waiting for response */
    PAIRED       /* Successfully paired with a partner */
} BROADCAST_STATE;

typedef enum {
    MSG_HELLO = 0,      /* Broadcasting availability */
    MSG_PROPOSAL = 1,   /* Proposing to pair with a specific device */
    MSG_ACCEPT = 2,     /* Accepting a pairing proposal */
    MSG_REJECT = 3,     /* Rejecting a pairing proposal */
    MSG_HEARTBEAT = 4   /* Periodic heartbeat while paired */
} MSG_TYPE;

/* 
 * Broadcast packet structure for pairing protocol.
 * Each broadcast in espnow should cast from void to broadcast_t or vice versa.
 */
typedef struct {
    uint8_t protocol_id;                    /* Filter for our protocol (PAIRING_PROTOCOL_ID) */
    uint8_t msg_type;                       /* MSG_TYPE: HELLO, PROPOSAL, ACCEPT, REJECT, HEARTBEAT */
    uint8_t sender_mac[ESP_NOW_ETH_ALEN];   /* MAC address of the sender */
    uint8_t bitmask;                        /* Application-specific flags */
    uint8_t state;                          /* Sender's current BROADCAST_STATE */
    uint8_t partner_mac[ESP_NOW_ETH_ALEN];  /* MAC of current/proposed partner (0 if none) */
    uint32_t uptime_ms;                     /* Sender's uptime in milliseconds */
    uint16_t seq_num;                       /* Sequence number for heartbeats */
    int8_t last_rssi;                       /* Last received RSSI from partner */
} __attribute__((packed)) broadcast_t;

#endif /* STATES_H */




