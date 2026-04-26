// PROMPT-03: WebRTC стек через libpeer
#include "webrtc_streamer.h"
#include "signaling.h"
#include "camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "peer.h"
#include "peer_connection.h"
#include <string.h>
#include <inttypes.h>
#include "esp_timer.h"

static const char *TAG = "webrtc";

// ──────────────────────────────────────────────
// ABR (Adaptive Bitrate) constants
// ──────────────────────────────────────────────

#define ABR_MIN_BPS          500000U
#define ABR_MAX_BPS         8000000U
#define ABR_RAISE_DELAY_US  5000000LL   // 5 s without loss → raise bitrate

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

// Periodic IDR retry: fires up to IDR_RETRY_COUNT times, every IDR_RETRY_INTERVAL_MS,
// after each new WebRTC connection to ensure the Quest gets at least one good IDR.
#define IDR_RETRY_INTERVAL_MS  2000
#define IDR_RETRY_COUNT        5
static esp_timer_handle_t            g_idr_timer       = NULL;
static volatile int                  g_idr_retry_left  = 0;

// ABR state: 0 = not yet initialised (lazy-read from camera_get_bitrate on first RR)
static uint32_t g_abr_bitrate  = 0;
static int64_t  g_abr_good_us  = 0;    // start of the "no-loss" period (0 = not started)

// ──────────────────────────────────────────────
// libpeer callbacks
// ──────────────────────────────────────────────

// Periodic IDR retry timer callback.
// Runs as a periodic timer; decrements counter and requests IDR until count hits 0.
// NO ESP_LOGI here — timer task stack is small and _svfprintf_r overflows it.
static void idr_retry_timer_cb(void *arg)
{
    if (g_idr_retry_left <= 0) {
        if (g_idr_timer) esp_timer_stop(g_idr_timer);
        return;
    }
    g_idr_retry_left--;
    camera_request_idr();
    if (g_idr_retry_left == 0 && g_idr_timer) {
        esp_timer_stop(g_idr_timer);
    }
}

// Called by libpeer with each RTCP Receiver Report.
// fraction_loss is in [0,1]; fraction_lost/256 per spec, but libpeer divides by 256 already.
// Logic:
//   fraction_loss > 20/256 (~8%) → reduce bitrate 20%, request IDR
//   fraction_loss == 0 for ≥5 s  → raise bitrate 10%
//   otherwise (loss but < threshold) → reset raise timer
static void on_receiver_packet_loss_cb(float fraction_loss,
                                        uint32_t total_loss, void *userdata)
{
    (void)userdata;
    (void)total_loss;

    // Lazy initialise from encoder on first callback
    if (g_abr_bitrate == 0) g_abr_bitrate = camera_get_bitrate();
    if (g_abr_bitrate == 0) return;   // encoder not ready yet

    int64_t now = esp_timer_get_time();

    if (fraction_loss * 256.0f > 20.0f) {
        // Loss above threshold — reduce 20%
        uint32_t nb = (uint32_t)(g_abr_bitrate * 0.8f);
        if (nb < ABR_MIN_BPS) nb = ABR_MIN_BPS;
        if (nb != g_abr_bitrate) {
            g_abr_bitrate = nb;
            camera_set_bitrate(nb);
            camera_request_idr();   // IDR after bitrate drop accelerates decoder recovery
        }
        g_abr_good_us = 0;   // reset raise timer
    } else if (fraction_loss == 0.0f) {
        if (g_abr_good_us == 0) g_abr_good_us = now;
        if ((now - g_abr_good_us) >= ABR_RAISE_DELAY_US) {
            uint32_t nb = (uint32_t)(g_abr_bitrate * 1.1f);
            if (nb > ABR_MAX_BPS) nb = ABR_MAX_BPS;
            if (nb != g_abr_bitrate) {
                g_abr_bitrate = nb;
                camera_set_bitrate(nb);
            }
            g_abr_good_us = now;   // slide window: next raise in another 5 s
        }
    } else {
        g_abr_good_us = 0;   // some loss, but below threshold — reset raise timer
    }
}

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
        // DataChannel is NOT created here: SCTP socket is just created at this
        // point (not yet connected). peer_connection_create_datachannel would
        // fail with "sctp not connected".
        //
        // Instead, on_dc_open_cb fires ~50ms later when SCTP connects, and
        // sets g_dc_sid = 0 there (matching Quest's negotiated=true, id=0 channel).
        // We do NOT send DATA_CHANNEL_OPEN (peer_connection_create_datachannel)
        // because the Quest uses a pre-negotiated channel — sending OPEN would
        // be a protocol violation and may disrupt the Quest side.
        if (g_on_connected) g_on_connected();

        // Start periodic IDR retry: fires IDR_RETRY_COUNT times, every
        // IDR_RETRY_INTERVAL_MS, so the Quest gets at least one clean IDR
        // even if the first burst is partially lost over WiFi.
        if (g_idr_timer) {
            esp_timer_stop(g_idr_timer);  // no-op if not running
            g_idr_retry_left = IDR_RETRY_COUNT;
            // Periodic timer: fires every IDR_RETRY_INTERVAL_MS; callback
            // decrements counter and stops the timer itself when count hits 0.
            esp_timer_start_periodic(g_idr_timer, IDR_RETRY_INTERVAL_MS * 1000ULL);
        }

    } else if (state == PEER_CONNECTION_FAILED ||
               state == PEER_CONNECTION_CLOSED ||
               state == PEER_CONNECTION_DISCONNECTED) {
        g_dc_ready = false;
        g_idr_retry_left = 0;
        if (g_idr_timer) esp_timer_stop(g_idr_timer);
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

// Called when the SCTP association is connected (sctp->connected = 1).
// This fires ~50ms after PEER_CONNECTION_COMPLETED once the SCTP handshake
// completes — it is NOT a DataChannel-level open event.
//
// We set g_dc_sid = 0 here: SCTP stream 0 matches the Quest's
// negotiated=true, id=0 DataChannel (WebRTC DTLS-client / offerer → even
// stream IDs; first = 0). No DATA_CHANNEL_OPEN message is needed because the
// Quest pre-negotiated the channel on its side.
static void on_dc_open_cb(void *userdata)
{
    (void)userdata;
    g_dc_sid   = 0;  // SCTP stream 0: offerer (ESP32) uses even IDs, first = 0
    g_dc_ready = true;
    ESP_LOGI(TAG, "SCTP connected — DataChannel ready on stream 0");
}

// Called by libpeer when the Quest sends PLI or FIR via RTCP.
// We request an IDR from the camera encoder so the Quest can start decoding.
static void on_keyframe_request_cb(void *userdata)
{
    (void)userdata;
    static int64_t s_last_idr_req_us = 0;
    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_last_idr_req_us) < 1500000LL) {
        ESP_LOGD(TAG, "PLI/FIR: rate-limited, skip IDR request");
        return;
    }
    s_last_idr_req_us = now_us;
    ESP_LOGI(TAG, "PLI/FIR received — requesting IDR");
    camera_request_idr();
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

    // Create the IDR retry periodic timer once (reused across reconnects).
    // Periodic avoids re-starting from within callback (which overflows timer stack).
    if (!g_idr_timer) {
        esp_timer_create_args_t timer_args = {
            .callback        = idr_retry_timer_cb,
            .arg             = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name            = "idr_retry",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&timer_args, &g_idr_timer);
    }

    peer_init();

    // ice_servers[5] left zero-initialized — AP mode, no STUN needed (same subnet)
    PeerConfiguration config = {
        .audio_codec           = CODEC_NONE,
        .video_codec           = CODEC_H264,
        .datachannel           = DATA_CHANNEL_STRING,
        .on_request_keyframe   = on_keyframe_request_cb,
    };

    g_pc = peer_connection_create(&config);
    if (!g_pc) {
        ESP_LOGE(TAG, "peer_connection_create failed");
        return;
    }

    peer_connection_onicecandidate(g_pc, on_ice_candidate_cb);
    peer_connection_oniceconnectionstatechange(g_pc, on_state_change_cb);
    peer_connection_ondatachannel(g_pc, on_dc_message_cb, on_dc_open_cb, on_dc_close_cb);
    peer_connection_on_receiver_packet_loss(g_pc, on_receiver_packet_loss_cb);

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
    ESP_LOGI(TAG, "Setting remote answer (%d bytes):\n%s", (int)strlen(sdp), sdp);
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
    PeerConnectionState cur_state = peer_connection_get_state(g_pc);
    static uint32_t s_frame_count = 0;
    static bool     s_was_completed = false;
    if (cur_state != PEER_CONNECTION_COMPLETED) {
        // Log once every 150 frames while waiting, so we know camera is alive
        static uint32_t s_wait_count = 0;
        if (++s_wait_count % 150 == 1) {
            ESP_LOGI(TAG, "push_video: waiting (state=%d total=%"PRIu32")",
                     (int)cur_state, s_wait_count);
        }
        return;
    }
    if (!s_was_completed) {
        s_was_completed = true;
        ESP_LOGI(TAG, "push_video: first frame after COMPLETED");
    }
    s_frame_count++;

    // Find first NAL after Annex-B start code (00 00 00 01 or 00 00 01)
    const uint8_t *nal = data;
    if (len > 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) nal += 4;
    else if (len > 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1)            nal += 3;
    uint8_t nal_type = nal[0] & 0x1f;

    if (nal_type == 5) {  // IDR
        ESP_LOGI(TAG, "push_video: IDR frame #%"PRIu32", len=%zu", s_frame_count, len);
    } else if (s_frame_count % 30 == 0) {
        ESP_LOGI(TAG, "push_video: frame #%"PRIu32" type=%d len=%zu", s_frame_count, nal_type, len);
    }

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
    g_idr_retry_left = 0;
    if (g_idr_timer) esp_timer_stop(g_idr_timer);
    g_abr_bitrate = 0;
    g_abr_good_us = 0;

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
