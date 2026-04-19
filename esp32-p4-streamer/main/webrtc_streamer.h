#pragma once

#include <stdint.h>
#include <stddef.h>

typedef void (*webrtc_on_connected_t)(void);
typedef void (*webrtc_on_disconnected_t)(void);
typedef void (*webrtc_on_datachannel_msg_t)(const char *msg, size_t len);

void webrtc_set_callbacks(
    webrtc_on_connected_t         on_connected,
    webrtc_on_disconnected_t      on_disconnected,
    webrtc_on_datachannel_msg_t   on_datachannel_msg
);

void webrtc_init(void);
void webrtc_create_offer(void);
void webrtc_set_answer(const char *sdp);
void webrtc_add_ice_candidate(const char *candidate,
                              int mline_index,
                              const char *sdp_mid);
void webrtc_push_video_frame(const uint8_t *data, size_t len, uint64_t pts_ms);
void webrtc_datachannel_send(const char *msg);
void webrtc_reset(void);
