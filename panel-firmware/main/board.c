#include "board.h"

#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "kart";

/* Pinler: V1.3 şematiğiyle doğrulanmış tablo (CROWPANEL_CONTEXT.md) ve Elecrow Lesson03 örneği */
#define PIN_SDA 15
#define PIN_SCL 16
#define STC8_ADDR 0x30
#define H_RES 800
#define V_RES 480
#define LVGL_BUF_ROWS 48

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_stc8;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static SemaphoreHandle_t s_lvgl_mutex;

/* ---------------- STC8H1K28 (arka ışık, dokunmatik etkinleştirme) ---------------- */
static esp_err_t stc8_write(uint8_t v)
{
    return i2c_master_transmit(s_stc8, &v, 1, 100);
}

void board_backlight_percent(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    stc8_write((uint8_t)(245 - (pct * 245 + 50) / 100));
}

const uint8_t *board_framebuffer(void)
{
    void *fb = NULL;
    if (!s_panel || esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK) return NULL;
    return fb;
}

/* ---------------- LVGL kilidi ---------------- */
bool lvgl_lock(int timeout_ms)
{
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mutex, t) == pdTRUE;
}

void lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

/* ---------------- LVGL ekran / dokunmatik ---------------- */
static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Çizim ölçümü (tanılama): tamamlanan kare sayısı, LVGL'nin meşgul süresi */
static volatile uint32_t s_perf_frames, s_perf_flushes, s_perf_busy_us, s_perf_t0_ms;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px);
    s_perf_flushes++;
    if (lv_display_flush_is_last(disp)) s_perf_frames++;
    lv_display_flush_ready(disp);
}

void board_perf_report(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t win = now - s_perf_t0_ms;
    uint32_t frames = s_perf_frames, flushes = s_perf_flushes, busy = s_perf_busy_us / 1000;
    s_perf_frames = s_perf_flushes = s_perf_busy_us = 0;
    s_perf_t0_ms = now;
    printf("PERF pencere=%u ms, kare=%u (%u.%u kare/sn), parca=%u, LVGL mesgul=%u ms (%%%u)\n", (unsigned)win, (unsigned)frames,
           (unsigned)(win ? frames * 1000 / win : 0), (unsigned)(win ? frames * 10000 / win % 10 : 0), (unsigned)flushes, (unsigned)busy,
           (unsigned)(win ? busy * 100 / win : 0));
    fflush(stdout);
}

/* Tanılama: sanal dokunma (PC'den "@? tap x y"); gerçek dokunma gibi 160 ms basılı kalır */
static volatile uint32_t s_inject_until_ms;
static volatile uint16_t s_inject_x, s_inject_y;

void board_inject_tap(int x, int y, int hold_ms)
{
    s_inject_x = (uint16_t)x;
    s_inject_y = (uint16_t)y;
    s_inject_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + (uint32_t)(hold_ms > 0 ? hold_ms : 160);
}

typedef struct { int x1, y1, x2, y2, ms, end_hold_ms; } drag_args_t;

static void drag_task(void *arg)
{
    drag_args_t d = *(drag_args_t *)arg;
    free(arg);
    if (d.ms < 1) d.ms = 1;
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        int el = (int)(now - t0);
        if (el >= d.ms) break;
        s_inject_x = (uint16_t)(d.x1 + (d.x2 - d.x1) * el / d.ms);
        s_inject_y = (uint16_t)(d.y1 + (d.y2 - d.y1) * el / d.ms);
        s_inject_until_ms = now + 80;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_inject_x = (uint16_t)d.x2;
    s_inject_y = (uint16_t)d.y2;
    s_inject_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + (uint32_t)(d.end_hold_ms > 0 ? d.end_hold_ms : 100);
    vTaskDelete(NULL);
}

void board_inject_drag(int x1, int y1, int x2, int y2, int ms, int end_hold_ms)
{
    drag_args_t *d = malloc(sizeof(*d));
    if (!d) return;
    *d = (drag_args_t){ x1, y1, x2, y2, ms, end_hold_ms };
    if (xTaskCreate(drag_task, "drag", 2048, d, 3, NULL) != pdPASS) free(d);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool was_down;
    static uint32_t last_down_ms;
    static uint16_t last_x, last_y;
    esp_lcd_touch_point_data_t pt[1];
    uint8_t n = 0;
    esp_lcd_touch_read_data(s_touch);
    bool touched = esp_lcd_touch_get_data(s_touch, pt, &n, 1) == ESP_OK && n > 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((int32_t)(s_inject_until_ms - now) > 0) {
        touched = true;
        pt[0].x = s_inject_x;
        pt[0].y = s_inject_y;
    }
    if (touched) {
        last_x = pt[0].x;
        last_y = pt[0].y;
        last_down_ms = now;
    } else if (was_down && now - last_down_ms < 60) {
        /* GT911 basılıyken bir okumada "dokunma yok" derse (sıçrama), 60 ms bırakma sayma: bir dokunuş iki tıklamaya bölünmesin */
        touched = true;
    }
    if (touched) {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_PRESSED;
        if (!was_down) {                 /* tanılama: dokunma başladığında koordinatı yaz */
            printf("@X\t%d\t%d\n", last_x, last_y);
            fflush(stdout);
        }
        was_down = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        was_down = false;
    }
}

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t wait = 10;
        if (lvgl_lock(-1)) {
            int64_t t0 = esp_timer_get_time();
            wait = lv_timer_handler();
            s_perf_busy_us += (uint32_t)(esp_timer_get_time() - t0);
            lvgl_unlock();
        }
        if (wait < 4) wait = 4;
        if (wait > 30) wait = 30;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

static esp_err_t lvgl_setup(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);

    size_t bytes = H_RES * LVGL_BUF_ROWS * sizeof(lv_color16_t);
    void *buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "LVGL çizim belleği ayrılamadı");

    lv_display_t *disp = lv_display_create(H_RES, V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf, NULL, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    if (s_touch) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_cb);
    }

    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12 * 1024, NULL, 4, NULL, 1) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "LVGL görevi oluşturulamadı");
    return ESP_OK;
}

/* ---------------- Kart başlatma ---------------- */
esp_err_t board_init(void)
{
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_mutex, ESP_ERR_NO_MEM, TAG, "kilit");

    /* I2C */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C hattı");
    const i2c_device_config_t stc8_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = STC8_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &stc8_cfg, &s_stc8), TAG, "STC8");
    vTaskDelay(pdMS_TO_TICKS(50));
    /* Elecrow örneği: 250 = dokunmatiği etkinleştir. Arka ışık, ilk çizimden sonra açılır. */
    if (stc8_write(250) != ESP_OK) ESP_LOGW(TAG, "STC8 (0x30) yanıt vermedi");
    vTaskDelay(pdMS_TO_TICKS(120));      /* dokunmatik sıfırlaması bitsin (Elecrow örneğinde de RGB başlatma süresi kadar bekleniyor) */
    stc8_write(245);

    /* RGB LCD (Elecrow Lesson03 zamanlamaları; IDF 6.1 alanlarına uyarlandı) */
    const esp_lcd_rgb_panel_config_t rgb = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = H_RES,
            .v_res = V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags = { .pclk_active_neg = 1 },
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .bounce_buffer_size_px = H_RES * 10,
        .dma_burst_size = 64,
        .hsync_gpio_num = GPIO_NUM_40,
        .vsync_gpio_num = GPIO_NUM_41,
        .de_gpio_num = GPIO_NUM_42,
        .pclk_gpio_num = GPIO_NUM_39,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            GPIO_NUM_21, GPIO_NUM_47, GPIO_NUM_48, GPIO_NUM_45, GPIO_NUM_38,        /* B0..B4 */
            GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, /* G0..G5 */
            GPIO_NUM_7, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_3, GPIO_NUM_46,          /* R0..R4 */
        },
        .flags = { .fb_in_psram = 1 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&rgb, &s_panel), TAG, "RGB panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel başlatma");

    /* GT911 (0x5D); reset/kesme pini kullanılmıyor (Elecrow örneğindeki gibi) */
    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &tp_io), TAG, "GT911 G/Ç");
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = H_RES,
        .y_max = V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 başlatılamadı: ekran çalışır, dokunma çalışmaz");
        s_touch = NULL;
    }

    return lvgl_setup();
}
