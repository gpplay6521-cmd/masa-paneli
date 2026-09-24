#pragma once
#include <stdbool.h>

/* Kurulum portalı: panel kendi Wi-Fi ağını açar (WPA2); bilgisayar/telefon bağlanınca "ağa giriş" (captive portal) sayfası açılır ve
 * Masa Paneli kurulum dosyası (MasaPaneli-Kurulum.cmd) panelin içinden indirilir. Böylece bilgisayarda hiçbir program olmasa da panel
 * kendi kurulumunu yanında taşır. Dosya yazılıma gömülüdür (main/install/, pc-helper\paketle.ps1 -Hafif üretir).
 * ESP-IDF'in captive_portal örneğine (DNS yönlendirme + 404'ü kök sayfaya çevirme + DHCP seçeneği 114) dayanır.
 * Wi-Fi yalnızca portal açıkken çalışır; kapatılınca sürücü bellekten tamamen kaldırılır. */

#define PORTAL_SSID "MasaPaneli-Kurulum"

typedef enum { PORTAL_OFF, PORTAL_STARTING, PORTAL_ON, PORTAL_FAILED } portal_state_t;

typedef struct {
    portal_state_t state;
    int clients;            /* ağa bağlı cihaz sayısı */
    int downloads;          /* tamamlanan kurulum dosyası indirmeleri (bu açılışta) */
    char ssid[33];
    char password[12];      /* 8 haneli rastgele sayı (her açılışta yeni) */
} portal_status_t;

/* Portalı aç/kapat isteği (engellemez: işi ayrı bir görev yapar). Otomatik kapanma: 10 dk cihaz bağlanmazsa/hareketsizse, en çok 60 dk. */
void portal_request(bool on);
void portal_get(portal_status_t *out);
