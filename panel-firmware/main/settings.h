#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Panel belleğinde (NVS) saklanan ayarlar. Tema panelde saklanır (varsayılan: Mercan). */
typedef struct {
    uint8_t theme;       /* 0..6, varsayılan 0 = Mercan */
    uint8_t brightness;  /* yüzde 10..100, varsayılan 80 */
    uint8_t dim_idx;     /* 0=30 sn, 1=2 dk, 2=10 dk, 3=kapalı; varsayılan 1 */
    uint8_t cover_color; /* 1 = vurgu rengi çalan şarkının kapağından gelir (açık/kapalı anahtarı); varsayılan 1 */
    uint8_t break_idx;   /* mola hatırlatıcı: 0=kapalı, 1=saat başı, 2=30, 3=45, 4=60, 5=90 dk (oturum süresi); varsayılan 1 */
} settings_t;

extern settings_t g_settings;

void settings_load(void);
void settings_save(void);
/* Boşta karartma süresi (ms); 0 = kapalı */
uint32_t settings_dim_ms(void);

/* Mola hatırlatıcı aralığı (dakika, oturum süresine göre); 0 = kapalı ya da saat başı kipi */
int settings_break_min(void);
/* true: her saat başında (yerel saat HH:00) hatırlat */
bool settings_break_hourly(void);
