#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* CrowPanel Advance 4.3" V1.3: I2C (SDA 15, SCL 16) + STC8H1K28 (0x30) + RGB LCD 800x480 + GT911 (0x5D) + LVGL */
esp_err_t board_init(void);

/* Arka ışık: yüzde 0..100. STC8 yazmacı: 0 = en parlak, 245 = kapalı (Elecrow örnek kodu, aralık 0–245).
 * ⚠ Ara değerlerin doğrusal parlaklık verdiği doğrulanmadı; cihazda gözle denenecek. */
void board_backlight_percent(int pct);

/* Tanılama: sanal dokunma (x, y ekran koordinatı, basılı kalma süresi ms) */
void board_inject_tap(int x, int y, int hold_ms);
/* Tanılama: sanal sürükleme; (x1,y1)'den (x2,y2)'ye süre_ms'de gider, sonra son konumda bekleme_ms basılı kalıp bırakır */
void board_inject_drag(int x1, int y1, int x2, int y2, int ms, int end_hold_ms);
/* Tanılama: önceki çağrıdan bu yana çizim kare sayısı ve LVGL'nin meşgul süresi (konsola yazar, sayaçları sıfırlar) */
void board_perf_report(void);

/* Tanılama: RGB panelin çerçeve tamponu (800x480 RGB565); yok ise NULL */
const uint8_t *board_framebuffer(void);

/* LVGL, kendi görevinde çalışır; başka görevlerden LVGL çağrısı yapmadan önce kilitle. */
bool lvgl_lock(int timeout_ms);
void lvgl_unlock(void);
