#pragma once
// TODO: implemented in PROMPT-05

#include <stdint.h>
#include <stddef.h>

void datachannel_on_open(void);
void datachannel_on_message(const char *json_str, size_t len);
void datachannel_on_close(void);

// Обновить encode_ms из camera_task (thread-safe)
void datachannel_update_encode_ms(uint32_t encode_ms);
