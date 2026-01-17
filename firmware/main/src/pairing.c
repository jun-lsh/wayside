#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "pairing.h"
#include "espnow.h"

static const char *TAG = "pairing";

#define HEADER_SIZE (sizeof(broadcast_header_t))

static void propose_pairing(pairing_ctx_t *ctx, const uint8_t *target_mac);
static void accept_pairing(pairing_ctx_t *ctx, const uint8_t *target_mac);
static void send_reject(pairing_ctx_t *ctx, const uint8_t *target_mac);
static void send_hello(pairing_ctx_t *ctx);
static void send_heartbeat(pairing_ctx_t *ctx);
static void handle_heartbeat(pairing_ctx_t *ctx, const uint8_t *mac_addr, const broadcast_header_t *pkt, int8_t rssi);
static void fill_packet_header(pairing_ctx_t *ctx, broadcast_header_t *pkt);
static void register_peer(const uint8_t *mac);
static uint32_t get_time_ms(void);

static size_t build_packet_with_bitmask(pairing_ctx_t *ctx, uint8_t *buf, size_t buf_size, 
                                        uint8_t msg_type, const char *pubkey);
static bool parse_incoming_packet(const uint8_t *data, int len,
                                  uint8_t **out_bitmask, uint16_t *out_bitmask_len,
                                  const char **out_pubkey);

esp_err_t pairing_init(pairing_ctx_t *ctx)
{
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Invalid context pointer");
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(pairing_ctx_t));
    ctx->current_state = SEARCHING;
    ctx->last_action_time = get_time_ms();
    
    ctx->has_bitmask = false;
    ctx->has_pubkey = false;
    ctx->bitmask = NULL;
    ctx->bitmask_len = 0;
    ctx->partner_bitmask = NULL;
    ctx->partner_bitmask_len = 0;
    
    memset(ctx->my_public_key, 0, PAIRING_KEY_MAX_LEN);
    memset(ctx->partner_public_key, 0, PAIRING_KEY_MAX_LEN);

    esp_err_t ret = esp_read_mac(ctx->my_mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Pairing initialized. Waiting for bitmask and pubkey via BLE...");
    return ESP_OK;
}

void pairing_set_pubkey(pairing_ctx_t *ctx, const char *pub_key)
{
    if (ctx == NULL || pub_key == NULL) return;

    strncpy(ctx->my_public_key, pub_key, PAIRING_KEY_MAX_LEN - 1);
    ctx->my_public_key[PAIRING_KEY_MAX_LEN - 1] = '\0';
    ctx->has_pubkey = true;
    
    if (pairing_is_ready(ctx)) {
        pairing_reset(ctx);
        ESP_LOGI(TAG, "Pubkey configured. Both ready - broadcasting STARTED.");
    } else {
        ESP_LOGI(TAG, "Pubkey configured. Waiting for bitmask.");
    }
}

void pairing_set_bitmask(pairing_ctx_t *ctx, const uint8_t *data, uint16_t len)
{
    if (ctx == NULL || data == NULL || len == 0 || len > PAIRING_BITMASK_MAX_LEN) return;

    if (ctx->bitmask != NULL) {
        free(ctx->bitmask);
    }
    
    ctx->bitmask = malloc(len);
    if (ctx->bitmask == NULL) {
        ESP_LOGE(TAG, "Failed to allocate bitmask");
        return;
    }
    
    memcpy(ctx->bitmask, data, len);
    ctx->bitmask_len = len;
    ctx->has_bitmask = true;
    
    if (pairing_is_ready(ctx)) {
        pairing_reset(ctx);
        ESP_LOGI(TAG, "Bitmask configured (%d bytes). Both ready - broadcasting STARTED.", len);
    } else {
        ESP_LOGI(TAG, "Bitmask configured (%d bytes). Waiting for pubkey.", len);
    }
}

bool pairing_is_ready(const pairing_ctx_t *ctx)
{
    return ctx != NULL && ctx->has_bitmask && ctx->has_pubkey;
}

void pairing_handle_recv(pairing_ctx_t *ctx, const uint8_t *mac_addr,
                         const uint8_t *data, int len, int8_t rssi)
{
    if (!pairing_is_ready(ctx)) return;
    if (ctx == NULL || mac_addr == NULL || data == NULL) return;
    if (len < HEADER_SIZE) return;

    const broadcast_header_t *pkt = (const broadcast_header_t *)data;

    if (pkt->protocol_id != PAIRING_PROTOCOL_ID) return;

    ESP_LOGD(TAG, "Recv from " MACSTR " type=%d state=%d rssi=%d",
             MAC2STR(mac_addr), pkt->msg_type, ctx->current_state, rssi);

    uint8_t *recv_bitmask = NULL;
    uint16_t recv_bitmask_len = 0;
    const char *recv_pubkey = NULL;
    
    if (!parse_incoming_packet(data, len, &recv_bitmask, &recv_bitmask_len, &recv_pubkey)) {
        ESP_LOGW(TAG, "Failed to parse packet");
        return;
    }

    switch (ctx->current_state) {

        case SEARCHING:
            if (pkt->msg_type == MSG_HELLO) {
                ESP_LOGI(TAG, "Received HELLO from " MACSTR ", proposing...", MAC2STR(mac_addr));
                
                if (recv_bitmask != NULL && recv_bitmask_len > 0) {
                    if (ctx->partner_bitmask != NULL) free(ctx->partner_bitmask);
                    ctx->partner_bitmask = malloc(recv_bitmask_len);
                    if (ctx->partner_bitmask != NULL) {
                        memcpy(ctx->partner_bitmask, recv_bitmask, recv_bitmask_len);
                        ctx->partner_bitmask_len = recv_bitmask_len;
                    }
                }
                
                propose_pairing(ctx, mac_addr);
            }
            else if (pkt->msg_type == MSG_PROPOSAL) {
                if (recv_pubkey != NULL && recv_bitmask != NULL) {
                    ESP_LOGI(TAG, "Received PROPOSAL from " MACSTR " with key+bitmask, accepting...", MAC2STR(mac_addr));
                    
                    strncpy(ctx->partner_public_key, recv_pubkey, PAIRING_KEY_MAX_LEN - 1);
                    ctx->partner_public_key[PAIRING_KEY_MAX_LEN - 1] = '\0';
                    
                    if (ctx->partner_bitmask != NULL) free(ctx->partner_bitmask);
                    ctx->partner_bitmask = malloc(recv_bitmask_len);
                    if (ctx->partner_bitmask != NULL) {
                        memcpy(ctx->partner_bitmask, recv_bitmask, recv_bitmask_len);
                        ctx->partner_bitmask_len = recv_bitmask_len;
                    }
                    
                    accept_pairing(ctx, mac_addr);
                } else {
                    ESP_LOGW(TAG, "Ignored PROPOSAL from " MACSTR " (missing payload)", MAC2STR(mac_addr));
                }
            }
            break;

        case PROPOSING:
            if (memcmp(ctx->partner_mac, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
                if (pkt->msg_type == MSG_ACCEPT) {
                    if (recv_pubkey != NULL) {
                        strncpy(ctx->partner_public_key, recv_pubkey, PAIRING_KEY_MAX_LEN - 1);
                        ctx->partner_public_key[PAIRING_KEY_MAX_LEN - 1] = '\0';
                        
                        if (recv_bitmask != NULL && recv_bitmask_len > 0) {
                            if (ctx->partner_bitmask != NULL) free(ctx->partner_bitmask);
                            ctx->partner_bitmask = malloc(recv_bitmask_len);
                            if (ctx->partner_bitmask != NULL) {
                                memcpy(ctx->partner_bitmask, recv_bitmask, recv_bitmask_len);
                                ctx->partner_bitmask_len = recv_bitmask_len;
                            }
                        }
                        
                        ctx->current_state = PAIRED;
                        uint32_t now = get_time_ms();
                        ctx->last_heartbeat_sent = now;
                        ctx->last_heartbeat_recv = now;
                        ctx->heartbeat_seq = 0;
                        ctx->partner_seq = 0;
                        ctx->missed_heartbeats = 0;
                        ctx->partner_rssi = rssi;
                        ESP_LOGI(TAG, ">>> PAIRED with " MACSTR, MAC2STR(ctx->partner_mac));
                    } else {
                        ESP_LOGW(TAG, "Ignored ACCEPT (missing pubkey)");
                    }
                }
                else if (pkt->msg_type == MSG_REJECT) {
                    ctx->current_state = SEARCHING;
                    ESP_LOGI(TAG, "<<< Rejected by " MACSTR ", back to searching", MAC2STR(mac_addr));
                }
            }
            else if (pkt->msg_type == MSG_PROPOSAL) {
                if (memcmp(mac_addr, ctx->my_mac, ESP_NOW_ETH_ALEN) > 0) {
                    ESP_LOGI(TAG, "Tie-breaker: accepting " MACSTR " (higher MAC)", MAC2STR(mac_addr));
                    if (recv_pubkey != NULL && recv_bitmask != NULL) {
                        strncpy(ctx->partner_public_key, recv_pubkey, PAIRING_KEY_MAX_LEN - 1);
                        ctx->partner_public_key[PAIRING_KEY_MAX_LEN - 1] = '\0';
                        
                        if (ctx->partner_bitmask != NULL) free(ctx->partner_bitmask);
                        ctx->partner_bitmask = malloc(recv_bitmask_len);
                        if (ctx->partner_bitmask != NULL) {
                            memcpy(ctx->partner_bitmask, recv_bitmask, recv_bitmask_len);
                            ctx->partner_bitmask_len = recv_bitmask_len;
                        }
                        
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
    if (!pairing_is_ready(ctx)) return;

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
    memset(ctx->partner_public_key, 0, PAIRING_KEY_MAX_LEN);
    
    if (ctx->partner_bitmask != NULL) {
        free(ctx->partner_bitmask);
        ctx->partner_bitmask = NULL;
    }
    ctx->partner_bitmask_len = 0;
    
    ctx->last_action_time = get_time_ms();
    ESP_LOGI(TAG, "Pairing reset to SEARCHING");
}

bool pairing_get_partner_key(const pairing_ctx_t *ctx, char *out_key, size_t max_len)
{
    if (ctx->current_state != PAIRED) return false;
    strncpy(out_key, ctx->partner_public_key, max_len);
    return true;
}

bool pairing_get_partner_bitmask(const pairing_ctx_t *ctx, uint8_t *out_data, uint16_t *out_len, uint16_t max_len)
{
    if (ctx->current_state != PAIRED || ctx->partner_bitmask == NULL) return false;
    
    uint16_t copy_len = ctx->partner_bitmask_len < max_len ? ctx->partner_bitmask_len : max_len;
    memcpy(out_data, ctx->partner_bitmask, copy_len);
    *out_len = copy_len;
    return true;
}

static size_t build_packet_with_bitmask(pairing_ctx_t *ctx, uint8_t *buf, size_t buf_size, 
                                        uint8_t msg_type, const char *pubkey)
{
    size_t pubkey_len = pubkey ? strlen(pubkey) + 1 : 0;
    size_t total_size = HEADER_SIZE + ctx->bitmask_len + pubkey_len;
    
    if (total_size > buf_size) {
        ESP_LOGE(TAG, "Packet too large: %d > %d", (int)total_size, (int)buf_size);
        return 0;
    }
    
    broadcast_header_t *pkt = (broadcast_header_t *)buf;
    memset(buf, 0, total_size);
    
    pkt->protocol_id = PAIRING_PROTOCOL_ID;
    pkt->msg_type = msg_type;
    fill_packet_header(ctx, pkt);
    pkt->bitmask_len = ctx->bitmask_len;
    
    uint8_t *payload = buf + HEADER_SIZE;
    
    if (ctx->bitmask_len > 0 && ctx->bitmask != NULL) {
        memcpy(payload, ctx->bitmask, ctx->bitmask_len);
        payload += ctx->bitmask_len;
    }
    
    if (pubkey != NULL) {
        memcpy(payload, pubkey, pubkey_len);
    }
    
    return total_size;
}

static bool parse_incoming_packet(const uint8_t *data, int len,
                                  uint8_t **out_bitmask, uint16_t *out_bitmask_len,
                                  const char **out_pubkey)
{
    if (len < HEADER_SIZE) return false;
    
    const broadcast_header_t *hdr = (const broadcast_header_t *)data;
    uint16_t bitmask_len = hdr->bitmask_len;
    
    if (bitmask_len > PAIRING_BITMASK_MAX_LEN) return false;
    if (HEADER_SIZE + bitmask_len > len) return false;
    
    const uint8_t *payload = data + HEADER_SIZE;
    
    if (bitmask_len > 0) {
        *out_bitmask = (uint8_t *)payload;
        *out_bitmask_len = bitmask_len;
        payload += bitmask_len;
    } else {
        *out_bitmask = NULL;
        *out_bitmask_len = 0;
    }
    
    size_t remaining = len - HEADER_SIZE - bitmask_len;
    if (remaining > 0) {
        *out_pubkey = (const char *)payload;
    } else {
        *out_pubkey = NULL;
    }
    
    return true;
}

static void fill_packet_header(pairing_ctx_t *ctx, broadcast_header_t *pkt)
{
    memcpy(pkt->sender_mac, ctx->my_mac, ESP_NOW_ETH_ALEN);
    memcpy(pkt->partner_mac, ctx->partner_mac, ESP_NOW_ETH_ALEN);
    pkt->state = ctx->current_state;
    pkt->uptime_ms = get_time_ms();
    pkt->last_rssi = ctx->partner_rssi;
}

static void send_hello(pairing_ctx_t *ctx)
{
    uint8_t buf[HEADER_SIZE + PAIRING_BITMASK_MAX_LEN];
    size_t pkt_size = build_packet_with_bitmask(ctx, buf, sizeof(buf), MSG_HELLO, NULL);
    
    if (pkt_size > 0) {
        esp_now_send(espnow_broadcast_mac, buf, pkt_size);
    }
}

static void send_heartbeat(pairing_ctx_t *ctx)
{
    broadcast_header_t pkt = {0};
    pkt.protocol_id = PAIRING_PROTOCOL_ID;
    pkt.msg_type = MSG_HEARTBEAT;
    pkt.seq_num = ctx->heartbeat_seq++;
    pkt.bitmask_len = 0;
    fill_packet_header(ctx, &pkt);

    esp_now_send(ctx->partner_mac, (uint8_t *)&pkt, HEADER_SIZE);
}

static void propose_pairing(pairing_ctx_t *ctx, const uint8_t *target_mac)
{
    memcpy(ctx->partner_mac, target_mac, ESP_NOW_ETH_ALEN);
    ctx->current_state = PROPOSING;
    ctx->last_action_time = get_time_ms();

    register_peer(target_mac);

    uint8_t buf[HEADER_SIZE + PAIRING_BITMASK_MAX_LEN + PAIRING_KEY_MAX_LEN];
    size_t pkt_size = build_packet_with_bitmask(ctx, buf, sizeof(buf), MSG_PROPOSAL, ctx->my_public_key);
    
    if (pkt_size > 0) {
        esp_err_t ret = esp_now_send(target_mac, buf, pkt_size);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "--> Sent PROPOSAL to " MACSTR, MAC2STR(target_mac));
        } else {
            ESP_LOGE(TAG, "Failed to send PROPOSAL: %s", esp_err_to_name(ret));
            ctx->current_state = SEARCHING;
        }
    }
}

static void accept_pairing(pairing_ctx_t *ctx, const uint8_t *target_mac)
{
    memcpy(ctx->partner_mac, target_mac, ESP_NOW_ETH_ALEN);
    ctx->current_state = PAIRED;
    
    uint32_t now = get_time_ms();
    ctx->last_heartbeat_sent = now;
    ctx->last_heartbeat_recv = now;
    ctx->heartbeat_seq = 0;

    register_peer(target_mac);

    uint8_t buf[HEADER_SIZE + PAIRING_BITMASK_MAX_LEN + PAIRING_KEY_MAX_LEN];
    size_t pkt_size = build_packet_with_bitmask(ctx, buf, sizeof(buf), MSG_ACCEPT, ctx->my_public_key);
    
    if (pkt_size > 0) {
        esp_err_t ret = esp_now_send(target_mac, buf, pkt_size);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, ">>> Sent ACCEPT to " MACSTR, MAC2STR(target_mac));
        } else {
            ESP_LOGE(TAG, "Failed to send ACCEPT: %s", esp_err_to_name(ret));
        }
    }
}

static void send_reject(pairing_ctx_t *ctx, const uint8_t *target_mac)
{
    register_peer(target_mac);

    broadcast_header_t pkt = {0};
    pkt.protocol_id = PAIRING_PROTOCOL_ID;
    pkt.msg_type = MSG_REJECT;
    pkt.bitmask_len = 0;
    fill_packet_header(ctx, &pkt);

    esp_now_send(target_mac, (uint8_t *)&pkt, HEADER_SIZE);
    ESP_LOGI(TAG, "<<< Sent REJECT to " MACSTR, MAC2STR(target_mac));
}

static void handle_heartbeat(pairing_ctx_t *ctx, const uint8_t *mac_addr, const broadcast_header_t *pkt, int8_t rssi)
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
