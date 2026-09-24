#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "proto.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "panel";

void app_main(void)
{
    /* Konsol UART0'da protokolle paylaşılıyor: yalnızca kendi iletilerimiz görünsün */
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    settings_load();
    ESP_ERROR_CHECK(board_init());

    if (lvgl_lock(-1)) {
        ui_init();
        lvgl_unlock();
    }
    proto_start();
    ESP_LOGI(TAG, "masa paneli hazır (tema %u, parlaklık %u%%)", g_settings.theme, g_settings.brightness);
}
