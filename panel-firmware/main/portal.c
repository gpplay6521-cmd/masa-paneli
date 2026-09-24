#include "portal.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dns_server.h"          /* esp_netif.h'dan sonra (esp_ip4_addr_t) */

static const char *TAG = "portal";

#define PORTAL_CHANNEL 6
#define PORTAL_MAX_STA 4
#define IDLE_STOP_MS (10 * 60 * 1000)        /* cihaz yokken/hareketsizken bu süre sonra kapanır */
#define HARD_STOP_MS (60 * 60 * 1000)

/* Yazılıma gömülü kurulum dosyası (CMakeLists: EMBED_FILES) */
extern const uint8_t inst_start[] asm("_binary_MasaPaneli_Kurulum_cmd_start");
extern const uint8_t inst_end[] asm("_binary_MasaPaneli_Kurulum_cmd_end");

static portal_status_t s;                     /* durum (görev yazar, arayüz okur) */
static volatile bool s_want;                  /* istenen durum */
static TaskHandle_t s_task;
static esp_netif_t *s_netif;
static bool s_stack_ready;                    /* netif/olay döngüsü bir kez kurulur */
static httpd_handle_t s_httpd;
static dns_server_handle_t s_dns;
static esp_event_handler_instance_t s_evt;
static uint32_t s_started_ms, s_activity_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- Web sayfası (Türkçe). Kurulum sonrası bilgisayar kendi ağına döner: kurulum internet ister (Python paketleri). ---- */
static const char PAGE[] =
    "<!doctype html><html lang=\"tr\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Masa Paneli kurulumu</title><style>"
    "body{font-family:Segoe UI,system-ui,sans-serif;background:#0d0e11;color:#f4f5f7;margin:0;padding:24px;line-height:1.5}"
    "main{max-width:640px;margin:0 auto}h1{font-size:26px;margin:0 0 6px}p{color:#a7abb6}"
    ".k{background:#14161c;border-radius:16px;padding:16px 20px;margin:14px 0}.k b{color:#f4f5f7}"
    "a.d{display:block;text-align:center;background:#c9cdea;color:#0d0e11;font-weight:700;font-size:20px;border-radius:14px;padding:16px;"
    "text-decoration:none;margin:18px 0}code{display:block;background:#1b1e26;padding:10px;border-radius:8px;color:#f4f5f7;overflow-x:auto;"
    "font-size:13px;white-space:pre-wrap;word-break:break-all}small{color:#8a8f9e}"
    "</style></head><body><main><h1>Masa Paneli kurulumu</h1>"
    "<p>Bu bilgisayarın panelle konuşabilmesi için küçük bir program kurulacak. Yönetici izni gerekmez.</p>"
    "<a class=\"d\" href=\"/MasaPaneli-Kurulum.cmd\" download>Kurulum dosyasını indir</a>"
    "<div class=\"k\"><b>1.</b> Dosyayı indir. Tarayıcı uyarırsa <b>Sakla</b> / <b>Yine de sakla</b> de.</div>"
    "<div class=\"k\"><b>2.</b> Bilgisayarı <b>kendi Wi-Fi ağına</b> (internete) geri bağla. Kurulum, Python paketlerini internetten indirir.</div>"
    "<div class=\"k\"><b>3.</b> İndirilen <b>MasaPaneli-Kurulum.cmd</b> dosyasına çift tıkla. Windows uyarırsa <b>Yine de çalıştır</b> de. "
    "Panel ekranı birkaç dakika içinde kendiliğinden bağlanır.</div>"
    "<p><small>Tarayıcı dosyayı engellerse PowerShell'e şunu yapıştır (indirir, çalıştırmaz):</small></p>"
    "<code>iwr http://192.168.4.1/MasaPaneli-Kurulum.cmd -OutFile \"$HOME\\Downloads\\MasaPaneli-Kurulum.cmd\"</code>"
    "</main></body></html>";

static esp_err_t root_get(httpd_req_t *req)
{
    s_activity_ms = now_ms();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t file_get(httpd_req_t *req)
{
    s_activity_ms = now_ms();
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"MasaPaneli-Kurulum.cmd\"");
    esp_err_t e = httpd_resp_send(req, (const char *)inst_start, inst_end - inst_start);
    if (e == ESP_OK) s.downloads++;
    ESP_LOGW(TAG, "kurulum dosyası gönderildi: %s (%u bayt)", e == ESP_OK ? "tamam" : "HATA", (unsigned)(inst_end - inst_start));
    return e;
}

/* Bilinmeyen her adres (Windows/Android/iOS ağ denetim adresleri dahil) kök sayfaya yönlenir: "ağa giriş" penceresi böyle açılır.
 * iOS yönlendirmede gövde ister. */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    s_activity_ms = now_ms();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, "Masa Paneli kurulum sayfasina yonlendiriliyor", HTTPD_RESP_USE_STRLEN);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s.clients++;
        s_activity_ms = now_ms();
        ESP_LOGW(TAG, "cihaz bağlandı (toplam %d)", s.clients);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s.clients > 0) s.clients--;
        s_activity_ms = now_ms();
        ESP_LOGW(TAG, "cihaz ayrıldı (toplam %d)", s.clients);
    }
}

static void teardown(void)
{
    if (s_dns) { stop_dns_server(s_dns); s_dns = NULL; }
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    if (s_evt) { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_evt); s_evt = NULL; }
    esp_wifi_stop();
    esp_wifi_deinit();
}

static bool do_start(void)
{
    if (!s_stack_ready) {
        esp_netif_init();
        esp_err_t e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
        s_netif = esp_netif_create_default_wifi_ap();
        if (!s_netif) return false;
        s_stack_ready = true;
    }
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi sürücüsü başlatılamadı (boş iç bellek %u)", (unsigned)before);
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);                 /* ayar dosyasına yazma */
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, &s_evt) != ESP_OK) goto fail;

    snprintf(s.password, sizeof(s.password), "%08u", (unsigned)(esp_random() % 100000000u));
    snprintf(s.ssid, sizeof(s.ssid), "%s", PORTAL_SSID);
    wifi_config_t wc = { 0 };
    memcpy(wc.ap.ssid, PORTAL_SSID, strlen(PORTAL_SSID));
    wc.ap.ssid_len = strlen(PORTAL_SSID);
    memcpy(wc.ap.password, s.password, strlen(s.password));
    wc.ap.channel = PORTAL_CHANNEL;
    wc.ap.max_connection = PORTAL_MAX_STA;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &wc) != ESP_OK || esp_wifi_start() != ESP_OK) goto fail;

    /* DHCP: (1) istemcilere DNS sunucusu olarak panelin kendisini bildir (varsayılan olarak DNS sunulmaz; yoksa istemci adları çözemez ve
     * DNS yönlendirmesi işlemez), (2) seçenek 114: modern istemciler "ağa giriş" adresini doğrudan buradan öğrenir.
     * Sıra ESP-IDF softap_sta örneğindeki gibi: durdur -> seçenekler -> başlat. */
    static char uri[] = "http://192.168.4.1";
    uint8_t offer_dns = 0x02;                                    /* dhcpserver.h OFFER_DNS */
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("192.168.4.1");
    esp_netif_dhcps_stop(s_netif);
    esp_err_t e1 = esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
    esp_err_t e2 = esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_err_t e3 = esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    esp_err_t e4 = esp_netif_dhcps_start(s_netif);
    if (e1 || e2 || e3 || e4) ESP_LOGW(TAG, "DHCP ayarı: dns-teklifi=%d dns=%d seçenek114=%d başlat=%d", e1, e2, e3, e4);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_open_sockets = 5;                                /* lwIP 10 yuva: 5 + 3 iç + 1 DNS */
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) goto fail;
    static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    static const httpd_uri_t file = { .uri = "/MasaPaneli-Kurulum.cmd", .method = HTTP_GET, .handler = file_get };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &file);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, not_found);

    dns_server_config_t dc = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dc);
    if (!s_dns) goto fail;

    ESP_LOGW(TAG, "portal açık: ağ '%s', şifre %s (iç bellek %u -> %u bayt boş)", s.ssid, s.password, (unsigned)before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
fail:
    teardown();
    return false;
}

static void portal_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        bool want = s_want;
        if (want && s.state != PORTAL_ON && s.state != PORTAL_STARTING) {
            s.state = PORTAL_STARTING;
            s.clients = 0;
            s.downloads = 0;
            s.password[0] = 0;
            if (do_start()) {
                s_started_ms = s_activity_ms = now_ms();
                s.state = PORTAL_ON;
            } else {
                s.state = PORTAL_FAILED;
                s_want = false;
            }
        } else if (!want && (s.state == PORTAL_ON || s.state == PORTAL_FAILED)) {
            if (s.state == PORTAL_ON) teardown();
            ESP_LOGW(TAG, "portal kapatıldı (indirilen: %d, iç bellek %u bayt boş)", s.downloads,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            s.state = PORTAL_OFF;
            s.clients = 0;
        } else if (s.state == PORTAL_ON) {                  /* kendiliğinden kapanma */
            uint32_t now = now_ms();
            bool idle = s.clients == 0 && now - s_activity_ms > IDLE_STOP_MS;
            if (idle || now - s_started_ms > HARD_STOP_MS) {
                ESP_LOGW(TAG, "portal kendiliğinden kapanıyor (%s)", idle ? "hareketsiz" : "süre doldu");
                s_want = false;
                teardown();
                s.state = PORTAL_OFF;
                s.clients = 0;
            }
        }
    }
}

void portal_request(bool on)
{
    if (!s_task) {
        if (xTaskCreate(portal_task, "portal", 6144, NULL, 3, &s_task) != pdPASS) {
            s.state = PORTAL_FAILED;
            return;
        }
    }
    s_want = on;
    if (on && s.state == PORTAL_FAILED) s.state = PORTAL_OFF;    /* yeniden denemeye izin ver */
    xTaskNotifyGive(s_task);
}

void portal_get(portal_status_t *out)
{
    *out = s;
}
