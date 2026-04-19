// TODO: implemented in PROMPT-04
#include "camera.h"
#include "esp_log.h"

static const char *TAG = "camera";

esp_err_t camera_init(camera_frame_cb_t frame_cb)
{
    (void)frame_cb;
    ESP_LOGI(TAG, "camera_init stub");
    return ESP_OK;
}

void camera_deinit(void)
{
    ESP_LOGI(TAG, "camera_deinit stub");
}
