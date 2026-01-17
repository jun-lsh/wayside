#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "pairing.h"
#include "espnow.h"

static const char *TAG = "pairing";

// Offset to the start of the payload data in the struct
#define HEADER_SIZE (sizeof(broadcast_t) - PAIRING_KEY_MAX_LEN)

static void propose_pairing(pairing_ctx_t *ctx, const uint8_t *target_mac);
static void accept_pairing(pairing_ctx_t *ctx, const uint8_t *target_mac);
static void send_reject(pairing_ctx_t *ctx, const uint8_t *target_mac);
static void send_hello(pairing_ctx_t *ctx);
static void send_heartbeat(pairing_ctx_t *ctx);
static void handle_heartbeat(pairing_ctx_t *ctx, const uint8_t *mac_addr, const broadcast_t *pkt, int8_t rssi);
static void fill_packet_header(pairing_ctx_t *ctx, broadcast_t *pkt);
static void register_peer(const uint8_t *mac);
static uint32_t get_time_ms(void);

esp_err_t pairing_init(pairing_ctx_t *ctx)
{
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Invalid context pointer");
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(pairing_ctx_t));
    ctx->current_state = SEARCHING;
    ctx->last_action_time = get_time_ms();
    
    // Explicitly disable broadcasting until BLE configures us
    ctx->is_broadcasting = false;
    memset(ctx->my_public_key, 0, PAIRING_KEY_MAX_LEN);
    memset(ctx->partner_public_key, 0, PAIRING_KEY_MAX_LEN);

    esp_err_t ret = esp_read_mac(ctx->my_mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Pairing initialized. Waiting for Public Key via BLE...");
    return ESP_OK;
}

void pairing_set_config(pairing_ctx_t *ctx, const char *pub_key)
{
    if (ctx == NULL || pub_key == NULL) return;

    // Store the key safely
    strncpy(ctx->my_public_key, pub_key, PAIRING_KEY_MAX_LEN - 1);
    ctx->my_public_key[PAIRING_KEY_MAX_LEN - 1] = '\0';

    // Enable the state machine
    ctx->is_broadcasting = true;
    
    // Reset state to ensure a clean start with the new key
    pairing_reset(ctx);
    
    ESP_LOGI(TAG, "Public Key Configured. ESP-NOW Broadcasting STARTED.");
}

void pairing_handle_recv(pairing_ctx_t *ctx, const uint8_t *mac_addr,
                         const uint8_t *data, int len, int8_t rssi)
{
    // Ignore everything if we haven't been configured via BLE yet
    if (!ctx->is_broadcasting) {
        return;
    }

    if (ctx == NULL || mac_addr == NULL || data == NULL) return;

    // Basic header check
    if (len < HEADER_SIZE) {
        return;
    }

    const broadcast_t *pkt = (const broadcast_t *)data;

    if (pkt->protocol_id != PAIRING_PROTOCOL_ID) {
        return;
    }

    ESP_LOGD(TAG, "Recv from " MACSTR " type=%d state=%d rssi=%d",
             MAC2STR(mac_addr), pkt->msg_type, ctx->current_state, rssi);

    switch (ctx->current_state) {

        case SEARCHING:
            if (pkt->msg_type == MSG_HELLO) {
                // Received HELLO, start proposal (includes sending our Key)
                ESP_LOGI(TAG, "Received HELLO from " MACSTR ", proposing...", MAC2STR(mac_addr));
                propose_pairing(ctx, mac_addr);
            }
            else if (pkt->msg_type == MSG_PROPOSAL) {
                // Received PROPOSAL, extract their key and accept
                if (len > HEADER_SIZE) {
                     ESP_LOGI(TAG, "Received PROPOSAL from " MACSTR " with Key, accepting...", MAC2STR(mac_addr));
                     
                     // Store Partner Key
                     strncpy(ctx->partner_public_key, pkt->public_key, PAIRING_KEY_MAX_LEN - 1);
                     ctx->partner_public_key[PAIRING_KEY_MAX_LEN - 1] = '\0';
                     
                     accept_pairing(ctx, mac_addr);
                } else {
                    ESP_LOGW(TAG, "Ignored PROPOSAL from " MACSTR " (Missing Key Payload)", MAC2STR(mac_addr));
                }
            }
            break;

        case PROPOSING:
            if (memcmp(ctx->partner_mac, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
                if (pkt->msg_type == MSG_ACCEPT) {
                    
                    // Received ACCEPT, extract their key and pair
                    if (len > HEADER_SIZE) {
                        strncpy(ctx->partner_public_key, pkt->public_key, PAIRING_KEY_MAX_LEN - 1);
                        ctx->partner_public_key[PAIRING_KEY_MAX_LEN - 1] = '\0';
                        
                        ctx->current_state = PAIRED;
                        uint32_t now = get_time_ms();
                        ctx->last_heartbeat_sent = now;
                        ctx->last_heartbeat_recv = now;
                        ctx->heartbeat_seq = 0;
                        ctx->partner_seq = 0;
                        ctx->missed_heartbeats = 0;
                        ctx->partner_rssi = rssi;
                        ESP_LOGW(TAG, "PARTNER PUBKEY (truncated): %.50s...", ctx->partner_public_key); 
                        ESP_LOGI(TAG, ">>> PAIRED with " MACSTR " (Key Exchanged)", MAC2STR(ctx->partner_mac));
                    } else {
                        ESP_LOGW(TAG, "Ignored ACCEPT (Missing Key Payload)");
                    }
                }
                else if (pkt->msg_type == MSG_REJECT) {
                    ctx->current_state = SEARCHING;
                    ESP_LOGI(TAG, "<<< Rejected by " MACSTR ", back to searching", MAC2STR(mac_addr));
                }
            }
            else if (pkt->msg_type == MSG_PROPOSAL) {
                // Tie-breaker Logic
                if (memcmp(mac_addr, ctx->my_mac, ESP_NOW_ETH_ALEN) > 0) {
                    ESP_LOGI(TAG, "Tie-breaker: accepting " MACSTR " (higher MAC)", MAC2STR(mac_addr));
                    // Store Key from tie-breaker proposal
                    if (len > HEADER_SIZE) {
                        strncpy(ctx->partner_public_key, pkt->public_key, PAIRING_KEY_MAX_LEN - 1);
                        ctx->partner_public_key[PAIRING_KEY_MAX_LEN - 1] = '\0';
                        accept_pairing(ctx, mac_addr);
                    }
                } else {
                    ESP_LOGI(TAG, "Tie-breaker: rejecting " MACSTR " (lower MAC)", MAC2STR(mac_addr));
                    send_reject(ctx, mac_addr);
                }
            }
            break;

        case PAIRED:
            if (pkt->msg_type == MSG_HEARTBEAT) {
                if (memcmp(ctx->partner_mac, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
                    handle_heartbeat(ctx, mac_addr, pkt, rssi);
                }
            }
            else if (pkt->msg_type == MSG_PROPOSAL) {
                if (memcmp(ctx->partner_mac, mac_addr, ESP_NOW_ETH_ALEN) != 0) {
                    send_reject(ctx, mac_addr);
                }
            }
            break;
    }
}

void pairing_tick(pairing_ctx_t *ctx)
{
    if (ctx == NULL) return;
    
    // Do nothing if key is not configured yet
    if (!ctx->is_broadcasting) return;

    uint32_t now = get_time_ms();

    switch (ctx->current_state) {
        case SEARCHING:
            if (now - ctx->last_action_time > PAIRING_REBROADCAST_MS) {
                send_hello(ctx);
                ctx->last_action_time = now;
            }
            break;

        case PROPOSING:
            if (now - ctx->last_action_time > PAIRING_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Proposal timed out, resetting");
                ctx->current_state = SEARCHING;
                ctx->last_action_time = now;
            }
            break;

        case PAIRED:
            if (now - ctx->last_heartbeat_sent > PAIRING_HEARTBEAT_MS) {
                send_heartbeat(ctx);
                ctx->last_heartbeat_sent = now;
            }
            if (now - ctx->last_heartbeat_recv > PAIRING_HEARTBEAT_MS * PAIRING_HEARTBEAT_MISS_MAX) {
                ESP_LOGW(TAG, "Lost connection to partner");
                pairing_reset(ctx);
            }
            break;
    }
}

void pairing_reset(pairing_ctx_t *ctx)
{
    if (ctx == NULL) return;
    ctx->current_state = SEARCHING;
    memset(ctx->partner_mac, 0, ESP_NOW_ETH_ALEN);
    memset(ctx->partner_public_key, 0, PAIRING_KEY_MAX_LEN); // Clear partner key on reset
    ctx->last_action_time = get_time_ms();
    ESP_LOGI(TAG, "Pairing reset to SEARCHING");
}

bool pairing_get_partner_key(const pairing_ctx_t *ctx, char *out_key, size_t max_len)
{
    if (ctx->current_state != PAIRED) return false;
    strncpy(out_key, ctx->partner_public_key, max_len);
    return true;
}

// --- SEND HELPERS ---

static void fill_packet_header(pairing_ctx_t *ctx, broadcast_t *pkt)
{
    memcpy(pkt->sender_mac, ctx->my_mac, ESP_NOW_ETH_ALEN);
    memcpy(pkt->partner_mac, ctx->partner_mac, ESP_NOW_ETH_ALEN);
    pkt->bitmask = ctx->bitmask;
    pkt->state = ctx->current_state;
    pkt->uptime_ms = get_time_ms();
    pkt->last_rssi = ctx->partner_rssi;
}

static void send_hello(pairing_ctx_t *ctx)
{
    broadcast_t pkt = {0};
    pkt.protocol_id = PAIRING_PROTOCOL_ID;
    pkt.msg_type = MSG_HELLO;
    fill_packet_header(ctx, &pkt);

    // Send only header size for HELLO (Key not needed)
    esp_now_send(espnow_broadcast_mac, (uint8_t *)&pkt, HEADER_SIZE);
}

static void send_heartbeat(pairing_ctx_t *ctx)
{
    broadcast_t pkt = {0};
    pkt.protocol_id = PAIRING_PROTOCOL_ID;
    pkt.msg_type = MSG_HEARTBEAT;
    pkt.seq_num = ctx->heartbeat_seq++;
    fill_packet_header(ctx, &pkt);

    // Send only header size for HEARTBEAT
    esp_now_send(ctx->partner_mac, (uint8_t *)&pkt, HEADER_SIZE);
}

static void propose_pairing(pairing_ctx_t *ctx, const uint8_t *target_mac)
{
    memcpy(ctx->partner_mac, target_mac, ESP_NOW_ETH_ALEN);
    ctx->current_state = PROPOSING;
    ctx->last_action_time = get_time_ms();

    register_peer(target_mac);

    broadcast_t pkt = {0};
    pkt.protocol_id = PAIRING_PROTOCOL_ID;
    pkt.msg_type = MSG_PROPOSAL;
    fill_packet_header(ctx, &pkt);

    // INCLUDE KEY: Copy local public key to packet
    strncpy(pkt.public_key, ctx->my_public_key, PAIRING_KEY_MAX_LEN);
    
    // Send Full Packet (Header + Key)
    esp_err_t ret = esp_now_send(target_mac, (uint8_t *)&pkt, sizeof(broadcast_t));
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "--> Sent PROPOSAL to " MACSTR " with Key", MAC2STR(target_mac));
    } else {
        ESP_LOGE(TAG, "Failed to send PROPOSAL: %s", esp_err_to_name(ret));
        ctx->current_state = SEARCHING;
    }
}

static void accept_pairing(pairing_ctx_t *ctx, const uint8_t *target_mac)
{
    memcpy(ctx->partner_mac, target_mac, ESP_NOW_ETH_ALEN);
    ctx->current_state = PAIRED;
    
    // Reset Heartbeat counters
    uint32_t now = get_time_ms();
    ctx->last_heartbeat_sent = now;
    ctx->last_heartbeat_recv = now;
    ctx->heartbeat_seq = 0;

    register_peer(target_mac);

    broadcast_t pkt = {0};
    pkt.protocol_id = PAIRING_PROTOCOL_ID;
    pkt.msg_type = MSG_ACCEPT;
    fill_packet_header(ctx, &pkt);

    // INCLUDE KEY: Copy local public key to packet
    strncpy(pkt.public_key, ctx->my_public_key, PAIRING_KEY_MAX_LEN);

    // Send Full Packet (Header + Key)
    esp_err_t ret = esp_now_send(target_mac, (uint8_t *)&pkt, sizeof(broadcast_t));
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, ">>> Sent ACCEPT to " MACSTR " with Key", MAC2STR(target_mac));
    } else {
        ESP_LOGE(TAG, "Failed to send ACCEPT: %s", esp_err_to_name(ret));
    }
}

static void send_reject(pairing_ctx_t *ctx, const uint8_t *target_mac)
{
    register_peer(target_mac);

    broadcast_t pkt = {0};
    pkt.protocol_id = PAIRING_PROTOCOL_ID;
    pkt.msg_type = MSG_REJECT;
    fill_packet_header(ctx, &pkt);

    // REJECT doesn't need key, send small packet
    esp_now_send(target_mac, (uint8_t *)&pkt, HEADER_SIZE);
    ESP_LOGI(TAG, "<<< Sent REJECT to " MACSTR, MAC2STR(target_mac));
}

static void handle_heartbeat(pairing_ctx_t *ctx, const uint8_t *mac_addr, const broadcast_t *pkt, int8_t rssi)
{
    ctx->last_heartbeat_recv = get_time_ms();
    ctx->missed_heartbeats = 0;
    ctx->partner_seq = pkt->seq_num;
    ctx->partner_rssi = rssi;
}

static void register_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;

    esp_now_peer_info_t peer_info = {
        .channel = 0,
        .ifidx = ESPNOW_WIFI_IF,
        .encrypt = false,
    };
    memcpy(peer_info.peer_addr, mac, ESP_NOW_ETH_ALEN);
    esp_now_add_peer(&peer_info);
}

static uint32_t get_time_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}