#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct {
    const char *name;      /* Türkçe tema adı */
    uint32_t accent;       /* vurgu rengi */
    uint32_t deep;         /* başlık kartı ortası */
    uint32_t night;        /* başlık kartı koyu ucu */
} theme_t;

#define THEME_COUNT 7          /* seçilebilir sabit temalar */
extern const theme_t THEMES[THEME_COUNT];
extern theme_t g_theme_dyn;    /* "Kapak rengi" açıkken çalan şarkının kapağından üretilen renkler */

/* Kapağın (RGB565, w x h) baskın canlı renginden kapak renklerini üret. Vurgu rengi belirgin değiştiyse true.
 * Gri/renksiz kapakta nötr bir vurgu seçilir. */
bool theme_dyn_from_cover(const uint16_t *px, int w, int h);

/* Sabit renkler (tasarım tuvali ile aynı) */
#define COL_BG        0x0D0E11
#define COL_SURFACE   0x1B1E26
#define COL_CARD      0x14161C
#define COL_TRACK     0x2A2E39
#define COL_TEXT      0xF4F5F7
#define COL_SUB       0xA7ABB6
#define COL_DIM       0x8A8F9E
#define COL_IDLE_BG   0x050608
#define COL_AMBER     0xFFC857
#define COL_GREEN     0x5BD6A4
