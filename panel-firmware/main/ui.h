#pragma once

/* Arayüz (LVGL 9.1). ui_init() LVGL kilidi altında çağrılmalı.
 * Ekranlar: Şimdi çalıyor, Ayarlar, Tema, Boşta, Bağlantı yok, Sistem, Zamanlayıcı, Alarm. */
void ui_init(void);

/* Tanılama: etkin ekran (0 Şimdi çalıyor, 1 Ayarlar, 2 Tema, 3 Boşta, 4 Bağlantı yok, 5 Sistem, 6 Zamanlayıcı, 7 Alarm) */
int ui_screen_id(void);

/* Tanılama: belirtilen ekrana hemen geç (LVGL kilidini kendisi alır) */
void ui_debug_go(int screen_id);

/* Tanılama: mola hatırlatıcı aralığını geçici olarak sn cinsinden ayarla (0 = ayardaki değere dön) */
void ui_debug_break_interval(int seconds);

/* Tanılama: "saat başı" hatırlatmasının dakikasını ve en az oturum süresini (sn) değiştir; dakika < 0 ise varsayılana (0, 20 dk) döner */
void ui_debug_hourly(int minute, int min_session_s);
