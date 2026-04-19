// PROMPT-03: WebRTC стек через libpeer
#include "webrtc_streamer.h"
#include "signaling.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "peer.h"
#include "peer_connection.h"
#include <string.h>

static const char *TAG = "webrtc";

// ──────────────────────────────────────────────
// Globals
// ──────────────────────────────────────────────

static PeerConnection               *g_pc             = NULL;
static TaskHandle_t                  g_loop_task       = NULL;
static int                           g_dc_sid          = -1;
static volatile bool                 g_dc_ready        = false;
static webrtc_on_connected_t         g_on_connected    = NULL;
static webrtc_on_disconnected_t      g_on_disconnected = NULL;
static webrtc_on_datachannel_msg_t   g_on_dc_msg       = NULL;

// ──────────────────────────────────────────────
// libpeer callbacks
// ──────────────────────────────────────────────

// Called by libpeer when local SDP offer is ready (includes ICE host candidates).
// libpeer embeds all candidates in the SDP — this is NOT trickle ICE.
// We send the full SDP as the WebRTC offer to the viewer.
static void on_ice_candidate_cb(char *sdp, void *userdata)
{
    (void)userdata;
    if (!sdp) return;
    ESP_LOGI(TAG, "SDP offer ready (%d bytes), sending to viewer", (int)strlen(sdp));
    signaling_send_offer(sdp);
}

// Called when ICE/DTLS connection state changes.
static void on_state_change_cb(PeerConnectionState state, void *userdata)
{
    (void)userdata;
    ESP_LOGI(TAG, "ICE state: %s", peer_connection_state_to_string(state));

    if (state == PEER_CONNECTION_COMPLETED) {
        // Create DataChannel "fpv" now that DTLS + ICE are established
        int sid = peer_connection_create_datachannel(
            g_pc,
            DATA_CHANNEL_RELIABLE,  // ordered, reliable
            0,                       // priority
            0,                       // reliability_parameter
            "fpv",                   // label
            ""                       // protocol
        );
        if (sid < 0) {
            ESP_LOGE(TAG, "Failed to create DataChannel");
        } else {
            g_dc_sid = sid;
            ESP_LOGI(TAG, "DataChannel 'fpv' created, sid=%d", sid);
        }
        if (g_on_connected) g_on_connected();

    } else if (state == PEER_CONNECTION_FAILED ||
               state == PEER_CONNECTION_CLOSED ||
               state == PEER_CONNECTION_DISCONNECTED) {
        g_dc_ready = false;
        if (g_on_disconnected) g_on_disconnected();
    }
}

// Called when a DataChannel message arrives from the viewer.
static void on_dc_message_cb(char *msg, size_t len, void *userdata, uint16_t sid)
{
    (void)userdata;
    (void)sid;
    if (g_on_dc_msg) g_on_dc_msg(msg, len);
}

// Called when the DataChannel is open (after peer_connection_create_datachannel).
static void on_dc_open_cb(void *userdata)
{
    (void)userdata;
    g_dc_ready = true;
    ESP_LOGI(TAG, "DataChannel 'fpv' open");
}

// Called when the DataChannel closes.
static void on_dc_close_cb(void *userdata)
{
    (void)userdata;
    g_dc_ready = false;
    ESP_LOGI(TAG, "DataChannel 'fpv' closed");
}

// ──────────────────────────────────────────────
// Loop task — drives ICE, DTLS, SCTP internals
// ──────────────────────────────────────────────

static void peer_loop_task(void *arg)
{
    (void)arg;
    while (1) {
        if (g_pc) peer_connection_loop(g_pc);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

void webrtc_set_callbacks(
    webrtc_on_connected_t         on_connected,
    webrtc_on_disconnected_t      on_disconnected,
    webrtc_on_datachannel_msg_t   on_datachannel_msg)
{
    g_on_connected    = on_connected;
    g_on_disconnected = on_disconnected;
    g_on_dc_msg       = on_datachannel_msg;
}

void webrtc_init(void)
{
    ESP_LOGI(TAG, "webrtc_init");

    peer_init();

    // ice_servers[5] left zero-initialized — AP mode, no STUN needed (same subnet)
    PeerConfiguration config = {
        .audio_codec = CODEC_NONE,
        .video_codec = CODEC_H264,
        .datachannel = DATA_CHANNEL_STRING,
    };

    g_pc = peer_connection_create(&config);
    if (!g_pc) {
        ESP_LOGE(TAG, "peer_connection_create failed");
        return;
    }

    peer_connection_onicecandidate(g_pc, on_ice_candidate_cb);
    peer_connection_oniceconnectionstatechange(g_pc, on_state_change_cb);
    peer_connection_ondatachannel(g_pc, on_dc_message_cb, on_dc_open_cb, on_dc_close_cb);

    // Dedicated task drives ICE gathering, DTLS handshake, SCTP
    xTaskCreatePinnedToCore(peer_loop_task, "pc_loop", 8192, NULL, 6, &g_loop_task, 0);
    ESP_LOGI(TAG, "PeerConnection created, loop task started");
}

void webrtc_create_offer(void)
{
    if (!g_pc) {
        ESP_LOGW(TAG, "webrtc_create_offer: no PeerConnection");
        return;
    }
    ESP_LOGI(TAG, "Creating offer...");
    // ICE gathering (host candidates only) runs inside peer_connection_loop.
    // When complete, on_ice_candidate_cb is invoked with the full SDP offer.
    peer_connection_create_offer(g_pc);
}

void webrtc_set_answer(const char *sdp)
{
    if (!g_pc || !sdp) return;
    ESP_LOGI(TAG, "Setting remote answer (%d bytes)", (int)strlen(sdp));
    peer_connection_set_remote_description(g_pc, (char *)sdp);
}

void webrtc_add_ice_candidate(const char *candidate,
                              int mline_index,
                              const char *sdp_mid)
{
    (void)mline_index;
    (void)sdp_mid;
    if (!g_pc || !candidate) return;
    // libpeer takes only the candidate string; mline/mid are embedded in SDP
    peer_connection_add_ice_candidate(g_pc, (char *)candidate);
}

void webrtc_push_video_frame(const uint8_t *data, size_t len, uint64_t pts_ms)
{
    (void)pts_ms;
    if (!g_pc || !data || len == 0) return;
    // Only send when fully connected (ICE + DTLS established)
    if (peer_connection_get_state(g_pc) != PEER_CONNECTION_COMPLETED) return;
    peer_connection_send_video(g_pc, data, len);
}

void webrtc_datachannel_send(const char *msg)
{
    if (!g_pc || !g_dc_ready || g_dc_sid < 0 || !msg) return;
    peer_connection_datachannel_send_sid(g_pc, (char *)msg, strlen(msg),
                                         (uint16_t)g_dc_sid);
}

void webrtc_reset(void)
{
    ESP_LOGI(TAG, "webrtc_reset");

    g_dc_ready = false;
    g_dc_sid   = -1;

    if (g_loop_task) {
        vTaskDelete(g_loop_task);
        g_loop_task = NULL;
    }
    if (g_pc) {
        peer_connection_destroy(g_pc);
        g_pc = NULL;
    }
    peer_deinit();

    // Re-initialise for the next viewer
    webrtc_init();
}
