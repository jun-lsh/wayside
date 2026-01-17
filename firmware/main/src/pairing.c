#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "pairing.h"
#include "espnow.h"

static const char *TAG = "pairing";

static void propose_pairing(pairing_ctx_t *ctx, const uint8_t *target_mac);
static void accept_pairing(pairing_ctx_t *ctx, const uint8_t *target_mac);
static void send_reject(pairing_ctx_t *ctx, const uint8_t *target_mac);
static void send_hello(pairing_ctx_t *ctx);
static void send_heartbeat(pairing_ctx_t *ctx);
static void handle_heartbeat(pairing_ctx_t *ctx, const uint8_t *mac_addr, const broadcast_t *pkt, int8_t rssi);
static void fill_packet_context(pairing_ctx_t *ctx, broadcast_t *pkt);
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

    esp_err_t ret = esp_read_mac(ctx->my_mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Pairing initialized, MAC: " MACSTR, MAC2STR(ctx->my_mac));
    return ESP_OK;
}

void pairing_handle_recv(pairing_ctx_t *ctx, const uint8_t *mac_addr,
                         const uint8_t *data, int len, int8_t rssi)
{
    if (ctx == NULL || mac_addr == NULL || data == NULL) {
        ESP_LOGE(TAG, "Invalid arguments to pairing_handle_recv");
        return;
    }

    if (len != sizeof(broadcast_t)) {
        ESP_LOGD(TAG, "Invalid packet size: %d (expected %d)", len, sizeof(broadcast_t));
        return;
    }

    const broadcast_t *pkt = (const broadcast_t *)data;

    if (pkt->protocol_id != PAIRING_PROTOCOL_ID) {
        ESP_LOGD(TAG, "Ignoring packet with wrong protocol ID: 0x%02X", pkt->protocol_id);
        return;
    }

    ESP_LOGD(TAG, "Recv from " MACSTR " type=%d state=%d rssi=%d",
             MAC2STR(mac_addr), pkt->msg_type, ctx->current_state, rssi);

    switch (ctx->current_state) {

        case SEARCHING:
            if (pkt->msg_type == MSG_HELLO) {
                ESP_LOGI(TAG, "Received HELLO from " MACSTR ", proposing...", MAC2STR(mac_addr));
                propose_pairing(ctx, mac_addr);
            }
            else if (pkt->msg_type == MSG_PROPOSAL) {
                ESP_LOGI(TAG, "Received PROPOSAL from " MACSTR ", accepting...", MAC2STR(mac_addr));
                accept_pairing(ctx, mac_addr);
            }
            break;

        case PROPOSING:
            if (memcmp(ctx->partner_mac, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
                if (pkt->msg_type == MSG_ACCEPT) {
                    ctx->current_state = PAIRED;
                    
                    uint32_t now = get_time_ms();
                    ctx->last_heartbeat_sent = now;
                    ctx->last_heartbeat_recv = now;
                    ctx->heartbeat_seq = 0;
                    ctx->partner_seq = 0;
                    ctx->missed_heartbeats = 0;
                    ctx->partner_rssi = rssi;
                    
                    ESP_LOGI(TAG, ">>> PAIRED with " MACSTR, MAC2STR(ctx->partner_mac));
                }
                else if (pkt->msg_type == MSG_REJECT) {
                    ctx->current_state = SEARCHING;
                    ESP_LOGI(TAG, "<<< Rejected by " MACSTR ", back to searching", MAC2STR(mac_addr));
                }
            }
            else if (pkt->msg_type == MSG_PROPOSAL) {
                /*
                 * Tie-breaker: We're proposing to A, but B proposed to us.
                 * Rule: If B's MAC > our MAC, abort our proposal and accept B.
                 * This prevents deadlock when two devices propose to each other.
                 */
                if (memcmp(mac_addr, ctx->my_mac, ESP_NOW_ETH_ALEN) > 0) {
                    ESP_LOGI(TAG, "Tie-breaker: accepting " MACSTR " (higher MAC)", MAC2STR(mac_addr));
                    accept_pairing(ctx, mac_addr);
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
                    ESP_LOGI(TAG, "Rejecting proposal from " MACSTR " (already paired)", MAC2STR(mac_addr));
                    send_reject(ctx, mac_addr);
                }
            }
            break;
    }
}

void pairing_tick(pairing_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

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
                ESP_LOGW(TAG, "Proposal to " MACSTR " timed out, resetting", MAC2STR(ctx->partner_mac));
                ctx->current_state = SEARCHING;
                ctx->last_action_time = now;
            }
            break;

        case PAIRED:
            if (now - ctx->last_heartbeat_sent > PAIRING_HEARTBEAT_MS) {
                send_heartbeat(ctx);
                ctx->last_heartbeat_sent = now;
            }
            
            if (now - ctx->last_heartbeat_recv > PAIRING_HEARTBEAT_MS) {
                ctx->missed_heartbeats++;
                ctx->last_heartbeat_recv = now;
                
                if (ctx->missed_heartbeats >= PAIRING_HEARTBEAT_MISS_MAX) {
                    ESP_LOGW(TAG, "Lost connection to " MACSTR " (%d missed heartbeats)",
                             MAC2STR(ctx->partner_mac), ctx->missed_heartbeats);
                    pairing_reset(ctx);
                } else {
                    ESP_LOGD(TAG, "Missed heartbeat from partner (%d/%d)",
                             ctx->missed_heartbeats, PAIRING_HEARTBEAT_MISS_MAX);
                }
            }
            break;
    }
}

BROADCAST_STATE pairing_get_state(const pairing_ctx_t *ctx)
{
    if (ctx == NULL) {
        return SEARCHING;
    }
    return ctx->current_state;
}

bool pairing_is_paired(const pairing_ctx_t *ctx)
{
    if (ctx == NULL) {
        return false;
    }
    return ctx->current_state == PAIRED;
}

esp_err_t pairing_get_partner_mac(const pairing_ctx_t *ctx, uint8_t *out_mac)
{
    if (ctx == NULL || out_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->current_state != PAIRED) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(out_mac, ctx->partner_mac, ESP_NOW_ETH_ALEN);
    return ESP_OK;
}

void pairing_reset(pairing_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->current_state = SEARCHING;
    memset(ctx->partner_mac, 0, ESP_NOW_ETH_ALEN);
    ctx->last_action_time = get_time_ms();
    ESP_LOGI(TAG, "Pairing reset to SEARCHING");
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
    fill_packet_context(ctx, &pkt);

    esp_err_t ret = esp_now_send(target_mac, (uint8_t *)&pkt, sizeof(pkt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send PROPOSAL: %s", esp_err_to_name(ret));
        ctx->current_state = SEARCHING;
    } else {
        ESP_LOGI(TAG, "--> Sent PROPOSAL to " MACSTR, MAC2STR(target_mac));
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
    ctx->partner_seq = 0;
    ctx->missed_heartbeats = 0;
    ctx->partner_rssi = 0;

    register_peer(target_mac);

    broadcast_t pkt = {0};
    pkt.protocol_id = PAIRING_PROTOCOL_ID;
    pkt.msg_type = MSG_ACCEPT;
    fill_packet_context(ctx, &pkt);

    esp_err_t ret = esp_now_send(target_mac, (uint8_t *)&pkt, sizeof(pkt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send ACCEPT: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, ">>> Sent ACCEPT to " MACSTR ", now PAIRED", MAC2STR(target_mac));
    }
}

static void send_reject(pairing_ctx_t *ctx, const uint8_t *target_mac)
{
    register_peer(target_mac);

    broadcast_t pkt = {0};
    pkt.protocol_id = PAIRING_PROTOCOL_ID;
    pkt.msg_type = MSG_REJECT;
    fill_packet_context(ctx, &pkt);

    esp_err_t ret = esp_now_send(target_mac, (uint8_t *)&pkt, sizeof(pkt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send REJECT: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "<<< Sent REJECT to " MACSTR, MAC2STR(target_mac));
    }
}

static void send_hello(pairing_ctx_t *ctx)
{
    broadcast_t pkt = {0};
    pkt.protocol_id = PAIRING_PROTOCOL_ID;
    pkt.msg_type = MSG_HELLO;
    fill_packet_context(ctx, &pkt);

    esp_err_t ret = esp_now_send(espnow_broadcast_mac, (uint8_t *)&pkt, sizeof(pkt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send HELLO: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Sent HELLO broadcast");
    }
}

static void send_heartbeat(pairing_ctx_t *ctx)
{
    broadcast_t pkt = {0};
    pkt.protocol_id = PAIRING_PROTOCOL_ID;
    pkt.msg_type = MSG_HEARTBEAT;
    pkt.seq_num = ctx->heartbeat_seq++;
    fill_packet_context(ctx, &pkt);

    esp_err_t ret = esp_now_send(ctx->partner_mac, (uint8_t *)&pkt, sizeof(pkt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send HEARTBEAT: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Sent HEARTBEAT seq=%d to " MACSTR, pkt.seq_num, MAC2STR(ctx->partner_mac));
    }
}

static void handle_heartbeat(pairing_ctx_t *ctx, const uint8_t *mac_addr, const broadcast_t *pkt, int8_t rssi)
{
    ctx->last_heartbeat_recv = get_time_ms();
    ctx->missed_heartbeats = 0;
    ctx->partner_seq = pkt->seq_num;
    ctx->partner_rssi = rssi;
    
    ESP_LOGI(TAG, "Recv HEARTBEAT seq=%d from " MACSTR " | RSSI: %d dBm | uptime: %lu ms",
             pkt->seq_num, MAC2STR(mac_addr), rssi, (unsigned long)pkt->uptime_ms);
}

static void fill_packet_context(pairing_ctx_t *ctx, broadcast_t *pkt)
{
    memcpy(pkt->sender_mac, ctx->my_mac, ESP_NOW_ETH_ALEN);
    memcpy(pkt->partner_mac, ctx->partner_mac, ESP_NOW_ETH_ALEN);
    pkt->bitmask = ctx->bitmask;
    pkt->state = ctx->current_state;
    pkt->uptime_ms = get_time_ms();
    pkt->last_rssi = ctx->partner_rssi;
}

static void register_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) {
        return;
    }

    esp_now_peer_info_t peer_info = {
        .channel = 0,
        .ifidx = ESPNOW_WIFI_IF,
        .encrypt = false,
    };
    memcpy(peer_info.peer_addr, mac, ESP_NOW_ETH_ALEN);

    esp_err_t ret = esp_now_add_peer(&peer_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add peer " MACSTR ": %s", MAC2STR(mac), esp_err_to_name(ret));
    }
}

static uint32_t get_time_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}
