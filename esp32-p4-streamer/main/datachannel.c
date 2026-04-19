// TODO: implemented in PROMPT-05
#include "datachannel.h"
#include "esp_log.h"

static const char *TAG = "datachannel";

void datachannel_on_open(void)
{
    ESP_LOGI(TAG, "datachannel_on_open stub");
}

void datachannel_on_message(const char *json_str, size_t len)
{
    (void)json_str;
    (void)len;
}

void datachannel_on_close(void)
{
    ESP_LOGI(TAG, "datachannel_on_close stub");
}

void datachannel_update_encode_ms(uint32_t encode_ms)
{
    (void)encode_ms;
}
