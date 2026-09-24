#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "ayar";
settings_t g_settings = { .theme = 0, .brightness = 80, .dim_idx = 1, .cover_color = 1, .break_idx = 1 };

void settings_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS açılamadı (%s), varsayılan ayarlar", esp_err_to_name(err));
        return;
    }
    nvs_handle_t h;
    if (nvs_open("panel", NVS_READONLY, &h) != ESP_OK) {
        return; /* ilk açılış: varsayılanlar */
    }
    uint8_t v;
    if (nvs_get_u8(h, "theme", &v) == ESP_OK && v < 7) g_settings.theme = v;
    if (nvs_get_u8(h, "bri", &v) == ESP_OK && v >= 10 && v <= 100) g_settings.brightness = v;
    if (nvs_get_u8(h, "dim", &v) == ESP_OK && v < 4) g_settings.dim_idx = v;
    if (nvs_get_u8(h, "ccol", &v) == ESP_OK && v < 2) g_settings.cover_color = v;
    /* "brk2": eski "brk" anahtarı başka bir sıralamaydı (30 dk'dan başlıyordu); yeni anahtar varsayılanı "Saat başı" yapar */
    if (nvs_get_u8(h, "brk2", &v) == ESP_OK && v < 6) g_settings.break_idx = v;
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("panel", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "theme", g_settings.theme);
    nvs_set_u8(h, "bri", g_settings.brightness);
    nvs_set_u8(h, "dim", g_settings.dim_idx);
    nvs_set_u8(h, "ccol", g_settings.cover_color);
    nvs_set_u8(h, "brk2", g_settings.break_idx);
    nvs_commit(h);
    nvs_close(h);
}

uint32_t settings_dim_ms(void)
{
    static const uint32_t ms[4] = { 30000, 120000, 600000, 0 };
    return ms[g_settings.dim_idx < 4 ? g_settings.dim_idx : 3];
}

int settings_break_min(void)
{
    static const int mins[6] = { 0, 0, 30, 45, 60, 90 };
    return mins[g_settings.break_idx < 6 ? g_settings.break_idx : 0];
}

bool settings_break_hourly(void)
{
    return g_settings.break_idx == 1;
}
