#include "theme.h"

#include <math.h>

/* Sıra ve renkler tasarım tuvaliyle aynı; ilk tema (Mercan) varsayılan (kullanıcı kararı). */
const theme_t THEMES[THEME_COUNT] = {
    { "Mercan",   0xFF6F59, 0x3B2A8C, 0x1B1440 },
    { "Kehribar", 0xFFC24B, 0x8A2F3F, 0x3A1024 },
    { "Nane",     0x5BD6A4, 0x0F6B7A, 0x0A2F3A },
    { "Turkuaz",  0x3FD0E0, 0x2D4FBF, 0x111E52 },
    { "Gökyüzü",  0x7AA2FF, 0x5B3FCF, 0x1B1650 },
    { "Orkide",   0xB78CFF, 0x7A2E9E, 0x2A0F45 },
    { "Gül",      0xFF7EB6, 0x9C2E7A, 0x3A0F35 },
};

theme_t g_theme_dyn = { "Kapak rengi", 0xFF6F59, 0x3B2A8C, 0x1B1440 };

static void rgb_to_hsv(float r, float g, float b, float *h, float *s, float *v)
{
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 0) { *h = 0; return; }
    float hh;
    if (mx == r) hh = fmodf((g - b) / d, 6.0f);
    else if (mx == g) hh = (b - r) / d + 2.0f;
    else hh = (r - g) / d + 4.0f;
    hh *= 60.0f;
    *h = hh < 0 ? hh + 360.0f : hh;
}

static uint32_t hsv_to_rgb(float h, float s, float v)
{
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2.0f) - 1)), m = v - c, r, g, b;
    if (h < 60) { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    return ((uint32_t)((r + m) * 255.0f + 0.5f) << 16) | ((uint32_t)((g + m) * 255.0f + 0.5f) << 8) |
           (uint32_t)((b + m) * 255.0f + 0.5f);
}

static int color_dist(uint32_t a, uint32_t b)
{
    int d = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        int x = (int)((a >> sh) & 0xFF) - (int)((b >> sh) & 0xFF);
        d += x < 0 ? -x : x;
    }
    return d;
}

bool theme_dyn_from_cover(const uint16_t *px, int w, int h)
{
    enum { NB = 24 };                       /* renk tonu kovaları (15°) */
    float wsum[NB] = { 0 }, rs[NB] = { 0 }, gs[NB] = { 0 }, bs[NB] = { 0 };
    int n = 0;
    for (int y = 0; y < h; y += 2) {        /* her 2 pikselden biri yeter (4096 örnek) */
        for (int x = 0; x < w; x += 2) {
            uint16_t v = px[y * w + x];
            float r = (float)((v >> 11) & 31) / 31.0f, g = (float)((v >> 5) & 63) / 63.0f, b = (float)(v & 31) / 31.0f;
            float hh, ss, vv;
            rgb_to_hsv(r, g, b, &hh, &ss, &vv);
            n++;
            if (vv < 0.25f || ss < 0.25f) continue;          /* koyu ya da gri pikseller rengi belirlemez */
            float wg = ss * ss * vv;                           /* canlı renkler daha ağır basar */
            int k = ((int)(hh / 15.0f)) % NB;
            wsum[k] += wg; rs[k] += r * wg; gs[k] += g * wg; bs[k] += b * wg;
        }
    }
    int best = 0;
    float best_score = -1;
    for (int k = 0; k < NB; k++) {          /* komşu kovalarla birlikte en ağır ton */
        float sc = wsum[k] + 0.5f * (wsum[(k + NB - 1) % NB] + wsum[(k + 1) % NB]);
        if (sc > best_score) { best_score = sc; best = k; }
    }
    uint32_t accent, deep, night;
    if (n == 0 || best_score < 0.02f * (float)n) {          /* renksiz (gri/siyah-beyaz) kapak: nötr, hafif mavi-gri */
        accent = 0xB8C0D8;
        deep = hsv_to_rgb(225, 0.40f, 0.46f);
        night = hsv_to_rgb(228, 0.45f, 0.22f);
    } else {
        float ws = 0, r = 0, g = 0, b = 0;
        for (int d = -1; d <= 1; d++) {
            int k = (best + NB + d) % NB;
            ws += wsum[k]; r += rs[k]; g += gs[k]; b += bs[k];
        }
        float hh, ss, vv;
        rgb_to_hsv(r / ws, g / ws, b / ws, &hh, &ss, &vv);
        ss = fminf(fmaxf(ss, 0.55f), 0.90f);                 /* koyu zeminde canlı, üstündeki koyu yazı okunur kalsın */
        vv = 0.95f;
        accent = hsv_to_rgb(hh, ss, vv);
        deep = hsv_to_rgb(hh + 30.0f, 0.72f, 0.52f);
        night = hsv_to_rgb(hh + 35.0f, 0.75f, 0.22f);
    }
    bool changed = color_dist(accent, g_theme_dyn.accent) > 30;
    g_theme_dyn.accent = accent;
    g_theme_dyn.deep = deep;
    g_theme_dyn.night = night;
    return changed;
}
