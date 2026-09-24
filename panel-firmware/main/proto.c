#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "driver/uart.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fw_version.h"
#include "ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "proto";

#define COVER_BYTES (PROTO_COVER_W * PROTO_COVER_H * 2)
#define FRESH_WEATHER_MS (5 * 60 * 1000)
#define FRESH_SYS_MS 5000
#define FRESH_SESSION_MS 30000
#define SHOT_W 800
#define SHOT_H 480
#define SHOT_CHUNK 768

static proto_state_t s_state;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define SEEK_HOLD_MS 1500
static uint32_t s_seek_hold_ms;      /* bu ana kadar PC'nin konum bildirimi yok sayılır (panelden sarma sonrası) */
static uint32_t s_last_rx_ms;
static bool s_any_rx;
static volatile bool s_diag_req;
static volatile bool s_shot_req;

static bool s_have_time;
static int64_t s_time_base_ms;            /* yerel_epoch_ms - çalışma_süresi_ms */
static proto_weather_t s_weather;
static uint32_t s_weather_rx_ms;
static proto_sys_t s_sys;
static uint32_t s_sys_rx_ms;
static int32_t s_sess_base_s;             /* @U ile gelen oturum süresi */
static uint32_t s_sess_rx_ms;             /* ... ve geldiği an (0 = hiç gelmedi) */

/* Kapak: iki tampon dönüşümlü kullanılır; yayınlanan tampon, bir sonraki aktarım bitene kadar bozulmaz */
static uint8_t *s_cover_buf[2];
static int s_cover_pub = -1;              /* yayınlanan tampon (-1: yok) */
static bool s_cover_valid;
static uint32_t s_cover_seq;
static int s_rx_id = -1, s_rx_target;
static uint32_t s_rx_got, s_rx_crc;
static bool s_rx_bad;

/* Kurulum dosyası (USB-seri, Wi-Fi'siz bilgisayar için): main/install/MasaPaneli-Kurulum.cmd, CMakeLists EMBED_FILES */
extern const uint8_t inst_start[] asm("_binary_MasaPaneli_Kurulum_cmd_start");
extern const uint8_t inst_end[] asm("_binary_MasaPaneli_Kurulum_cmd_end");
static volatile bool s_inst_busy;
static proto_installer_t s_inst;

uint32_t proto_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int64_t now64_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Karakter sınırında keserek kopyala (UTF-8 bozulmasın) */
static void utf8_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---------------- base64 ---------------- */
static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Dönüş: çözülen bayt sayısı, hata = -1 */
static int b64_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        int v = b64val((unsigned char)*in);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

static size_t b64_encode(const uint8_t *in, size_t n, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    return o;
}

/* ---------------- kapak aktarımı ---------------- */
static void cover_begin(char **f, int n)
{
    if (n < 6) return;
    int w = atoi(f[2]), h = atoi(f[3]);
    s_rx_id = -1;
    if (w == 0 || h == 0) {                       /* kapak yok */
        taskENTER_CRITICAL(&s_lock);
        s_cover_valid = false;
        s_cover_seq++;
        taskEXIT_CRITICAL(&s_lock);
        return;
    }
    if (w != PROTO_COVER_W || h != PROTO_COVER_H || atoi(f[5]) != COVER_BYTES || !s_cover_buf[0]) return;
    s_rx_id = atoi(f[1]);
    s_rx_crc = (uint32_t)strtoul(f[4], NULL, 16);
    s_rx_got = 0;
    s_rx_bad = false;
    s_rx_target = (s_cover_pub == 0) ? 1 : 0;
}

static void cover_chunk(char **f, int n)
{
    if (n < 4 || atoi(f[1]) != s_rx_id || s_rx_bad) return;
    uint32_t off = (uint32_t)atoi(f[2]);
    if (off != s_rx_got) { s_rx_bad = true; return; }
    int got = b64_decode(f[3], s_cover_buf[s_rx_target] + off, COVER_BYTES - off);
    if (got < 0) { s_rx_bad = true; return; }
    s_rx_got += (uint32_t)got;
}

static void cover_end(char **f, int n)
{
    if (n < 2 || atoi(f[1]) != s_rx_id) return;
    int id = s_rx_id;
    s_rx_id = -1;
    if (s_rx_bad || s_rx_got != COVER_BYTES || esp_crc32_le(0, s_cover_buf[s_rx_target], COVER_BYTES) != s_rx_crc) {
        printf("@C\tcover_retry\t%d\n", id);
        fflush(stdout);
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    s_cover_pub = s_rx_target;
    s_cover_valid = true;
    s_cover_seq++;
    taskEXIT_CRITICAL(&s_lock);
}

/* ---------------- ekran görüntüsü (tanılama) ---------------- */
static void shot_task(void *arg)
{
    const size_t total = SHOT_W * SHOT_H * 2;
    const uint8_t *fb = board_framebuffer();
    uint8_t *copy = fb ? heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (copy && lvgl_lock(1000)) {
        memcpy(copy, fb, total);
        lvgl_unlock();
    } else if (copy) {
        free(copy);
        copy = NULL;
    }
    if (copy) {
        static char line[SHOT_CHUNK * 4 / 3 + 40];
        printf("@F\t%d\t%d\n", SHOT_W, SHOT_H);
        fflush(stdout);
        for (size_t off = 0, k = 0; off < total; off += SHOT_CHUNK, k++) {
            size_t n = total - off < SHOT_CHUNK ? total - off : SHOT_CHUNK;
            int len = snprintf(line, 24, "@f\t%u\t", (unsigned)off);
            len += (int)b64_encode(copy + off, n, line + len);
            line[len++] = '\n';
            fwrite(line, 1, (size_t)len, stdout);
            fflush(stdout);
            if ((k & 15) == 15) vTaskDelay(1);   /* boşta görevi ve gözcü zamanlayıcı aç kalmasın */
        }
        printf("@F\tend\n");
        fflush(stdout);
        free(copy);
    }
    s_shot_req = false;
    vTaskDelete(NULL);
}

/* ---------------- SHA-256 (tek parça, bağımsız) ----------------
 * Kurulum dosyasının PC'ye bozulmadan ulaştığını doğrulatmak için (PowerShell'in yerleşik "Get-FileHash -Algorithm
 * SHA256" ile karşılaştırılır). mbedtls'e bağımlı değil: ESP-IDF v6.1'de mbedtls'in gerçek "sha256.h" başlığı ana
 * bileşene açık değildi (yalnızca openthread'in özel iç kopyası görünüyordu) ve PRIV_REQUIRES eklemek, "main" bileşeninin
 * (REQUIRES verilmediği için) örtük olarak gördüğü diğer tüm bileşenleri (esp_wifi, esp_http_server, dns_server) görmez
 * hale getirirdi. FIPS 180-4'ün standart referans uygulaması. */
static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_transform(uint32_t st[8], const uint8_t data[64])
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) | ((uint32_t)data[i * 4 + 2] << 8) | data[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = SHA256_ROTR(w[i - 15], 7) ^ SHA256_ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = SHA256_ROTR(w[i - 2], 17) ^ SHA256_ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = st[0]; b = st[1]; c = st[2]; d = st[3]; e = st[4]; f = st[5]; g = st[6]; h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = SHA256_ROTR(e, 6) ^ SHA256_ROTR(e, 11) ^ SHA256_ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = SHA256_ROTR(a, 2) ^ SHA256_ROTR(a, 13) ^ SHA256_ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t st[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha256_transform(st, data + i);
    uint8_t buf[128] = { 0 };
    size_t rem = len - i;
    memcpy(buf, data + i, rem);
    buf[rem] = 0x80;
    size_t padded = (rem < 56) ? 64 : 128;
    uint64_t bitlen = (uint64_t)len * 8;
    for (int b2 = 0; b2 < 8; b2++) buf[padded - 1 - b2] = (uint8_t)(bitlen >> (8 * b2));
    sha256_transform(st, buf);
    if (padded == 128) sha256_transform(st, buf + 64);
    for (int j = 0; j < 8; j++) {
        out[j * 4] = (uint8_t)(st[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(st[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(st[j] >> 8);
        out[j * 4 + 3] = (uint8_t)(st[j]);
    }
}

/* ---------------- kurulum dosyası gönderimi (USB-seri, Wi-Fi'siz bilgisayar) ---------------- */
static void installer_task(void *arg)
{
    size_t total = (size_t)(inst_end - inst_start);
    unsigned char digest[32];
    sha256(inst_start, total, digest);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);

    taskENTER_CRITICAL(&s_lock);
    s_inst.state = INSTALLER_SENDING;
    s_inst.sent = 0;
    s_inst.total = (uint32_t)total;
    taskEXIT_CRITICAL(&s_lock);

    printf("@KBAS\t%u\t%s\n", (unsigned)total, hex);
    fflush(stdout);

    /* uart_write_bytes (VFS/stdio değil): printf/fwrite konsol sürücüsü LF'yi CRLF'e çevirir (terminal için), bu da
     * ham baytları bozardı (dosyadaki her "\n" "\r\n" olurdu). Ham aktarım bu yüzden doğrudan UART sürücüsüne yazılır. */
    const size_t CH = 4096;
    for (size_t off = 0; off < total; off += CH) {
        size_t n = total - off < CH ? total - off : CH;
        uart_write_bytes(UART_NUM_0, (const char *)(inst_start + off), n);
        taskENTER_CRITICAL(&s_lock);
        s_inst.sent = (uint32_t)(off + n);
        taskEXIT_CRITICAL(&s_lock);
        if (((off / CH) & 7) == 7) vTaskDelay(1);   /* gözcü zamanlayıcı ve LVGL için ara ver */
    }

    taskENTER_CRITICAL(&s_lock);
    s_inst.state = INSTALLER_DONE;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "kurulum dosyası USB-seri ile gönderildi (%u bayt)", (unsigned)total);
    s_inst_busy = false;
    vTaskDelete(NULL);
}

/* ---------------- satır işleme ---------------- */
static void handle_line(char *line)
{
    if (line[0] != '@') return;          /* ESP günlüğü ya da gürültü */
    char *f[16];
    int n = 0;
    f[n++] = line + 1;
    for (char *p = line + 1; *p && n < 16; p++) {
        if (*p == '\t') { *p = 0; f[n++] = p + 1; }
    }
    uint32_t now = proto_now_ms();

    taskENTER_CRITICAL(&s_lock);
    s_last_rx_ms = now;
    s_any_rx = true;
    taskEXIT_CRITICAL(&s_lock);

    switch (f[0][0]) {
    case 'H':
        break;
    case '?':
        if (n >= 2 && !strcmp(f[1], "shot")) {
            if (!s_shot_req) {
                s_shot_req = true;
                if (xTaskCreatePinnedToCore(shot_task, "shot", 6144, NULL, 2, NULL, 0) != pdPASS) s_shot_req = false;
            }
        } else if (n >= 3 && !strcmp(f[1], "go")) {
            ui_debug_go(atoi(f[2]));
        } else if (n >= 3 && !strcmp(f[1], "mola")) {
            ui_debug_break_interval(atoi(f[2]));
        } else if (n >= 4 && !strcmp(f[1], "saat")) {
            ui_debug_hourly(atoi(f[2]), atoi(f[3]));
        } else if (n >= 4 && !strcmp(f[1], "tap")) {
            board_inject_tap(atoi(f[2]), atoi(f[3]), n >= 5 ? atoi(f[4]) : 160);
        } else if (n >= 7 && !strcmp(f[1], "drag")) {      /* @? drag x1 y1 x2 y2 süre_ms [son_konumda_bekleme_ms] */
            board_inject_drag(atoi(f[2]), atoi(f[3]), atoi(f[4]), atoi(f[5]), atoi(f[6]), n >= 8 ? atoi(f[7]) : 300);
        } else if (n >= 2 && !strcmp(f[1], "perf")) {      /* çizim ölçümü: önceki "perf"ten bu yana kare sayısı ve LVGL'nin meşgul süresi */
            board_perf_report();
        } else {
            s_diag_req = true;
        }
        break;
    case 'S':
        if (n >= 4) {
            const char *title = n > 4 ? f[4] : "";
            const char *artist = n > 5 ? f[5] : "";
            taskENTER_CRITICAL(&s_lock);
            /* Kapak parça değişince burada silinmez: bilgisayar (yardımcı program) kapağı yönetir; aynı albümde kapak kesintisiz kalır,
             * farklıysa yardımcı program eski kapağı @G ile kaldırıp yenisini gönderir. */
            /* Panelden sarma yapıldıysa, PC'nin sarmayı işlemeden önce gönderdiği eski konumu kısa süre yok say (çubuk geri sıçramasın);
             * parça değiştiyse bekletme yok */
            bool hold_pos = (int32_t)(s_seek_hold_ms - now) > 0 && !strcmp(s_state.title, title);
            s_state.state = atoi(f[1]);
            if (!hold_pos) {
                s_state.pos_ms = atoi(f[2]);
                s_state.pos_tick_ms = now;
            }
            s_state.dur_ms = atoi(f[3]);
            utf8_copy(s_state.title, sizeof(s_state.title), title);
            utf8_copy(s_state.artist, sizeof(s_state.artist), artist);
            s_state.seq++;
            taskEXIT_CRITICAL(&s_lock);
        }
        break;
    case 'T':
        if (n >= 2) {
            int64_t epoch = atoll(f[1]);
            taskENTER_CRITICAL(&s_lock);
            s_time_base_ms = epoch * 1000 - now64_ms();
            s_have_time = true;
            taskEXIT_CRITICAL(&s_lock);
        }
        break;
    case 'W':
        if (n >= 9) {
            proto_weather_t w = { 0 };
            w.valid = f[1][0] != 0;
            utf8_copy(w.code, sizeof(w.code), f[1]);
            w.temp = atoi(f[2]);
            w.feels = atoi(f[3]);
            w.hum = atoi(f[4]);
            w.tmin = atoi(f[5]);
            w.tmax = atoi(f[6]);
            w.night = atoi(f[7]) != 0;
            utf8_copy(w.place, sizeof(w.place), f[8]);
            taskENTER_CRITICAL(&s_lock);
            s_weather = w;
            s_weather_rx_ms = now;
            taskEXIT_CRITICAL(&s_lock);
        }
        break;
    case 'U':
        if (n >= 2) {
            taskENTER_CRITICAL(&s_lock);
            s_sess_base_s = atoi(f[1]);
            s_sess_rx_ms = now ? now : 1;
            taskEXIT_CRITICAL(&s_lock);
        }
        break;
    case 'M':
        if (n >= 12) {
            taskENTER_CRITICAL(&s_lock);
            s_sys.cpu = atoi(f[1]);
            s_sys.cpu_temp = atoi(f[2]);
            s_sys.ram = atoi(f[3]);
            s_sys.ram_used_mb = atoi(f[4]);
            s_sys.ram_total_mb = atoi(f[5]);
            s_sys.gpu = atoi(f[6]);
            s_sys.gpu_temp = atoi(f[7]);
            s_sys.vram = atoi(f[8]);
            s_sys.vram_used_mb = atoi(f[9]);
            s_sys.vram_total_mb = atoi(f[10]);
            s_sys.gpu_power = atoi(f[11]);
            s_sys.net_down_kbps = n >= 15 ? atoi(f[12]) : PROTO_NA;
            s_sys.net_up_kbps = n >= 15 ? atoi(f[13]) : PROTO_NA;
            s_sys.ping_ms = n >= 15 ? atoi(f[14]) : PROTO_NA;
            s_sys_rx_ms = now;
            taskEXIT_CRITICAL(&s_lock);
        }
        break;
    case 'N':
        if (n >= 5) {
            taskENTER_CRITICAL(&s_lock);
            utf8_copy(s_sys.host, sizeof(s_sys.host), f[1]);
            utf8_copy(s_sys.cpu_name, sizeof(s_sys.cpu_name), f[2]);
            utf8_copy(s_sys.gpu_name, sizeof(s_sys.gpu_name), f[3]);
            s_sys.threads = atoi(f[4]);
            taskEXIT_CRITICAL(&s_lock);
        }
        break;
    case 'G': cover_begin(f, n); break;
    case 'I': cover_chunk(f, n); break;
    case 'E': cover_end(f, n); break;
    case 'K':
        if (!strcmp(f[0], "KAL") && !s_inst_busy) {
            s_inst_busy = true;
            if (xTaskCreatePinnedToCore(installer_task, "installer", 4096, NULL, 2, NULL, 0) != pdPASS) s_inst_busy = false;
        }
        break;
    default:
        break;
    }
}

static void rx_task(void *arg)
{
    uint8_t buf[256];
    static char line[1100];
    size_t len = 0;
    bool drop = false;
    for (;;) {
        int n = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n') {
                line[len] = 0;
                if (!drop) handle_line(line);
                if (s_diag_req) {
                    s_diag_req = false;
                    proto_state_t d;
                    proto_get(&d);
                    printf("@!\t%lu\t%d\t%d\t%d\t%u\t%u\t%s\n", (unsigned long)d.seq, d.state, d.alive, ui_screen_id(),
                           (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), FW_VERSION);
                    fflush(stdout);
                }
                len = 0;
                drop = false;
            } else if (c == '\r') {
                continue;
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
            } else {
                drop = true;     /* çok uzun satır: bir sonraki satıra kadar at */
            }
        }
    }
}

void proto_start(void)
{
    /* Konsol UART0'da (CH340K); yalnızca okuma için sürücü kuruyoruz, yazma printf ile (satır bölünmesin).
     * 921600 bps: 128x128 kapak (32 KB) yarım saniyede geçer; CH340K en çok 2 Mbps destekler (WCH veri sayfası). */
    if (!uart_is_driver_installed(UART_NUM_0)) {
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 8192, 0, 0, NULL, 0));
    }
    uart_set_baudrate(UART_NUM_0, PROTO_BAUD);
    uart_set_rx_full_threshold(UART_NUM_0, 32);     /* FIFO 128 bayt: erken boşalt, hızlı akışta bayt kaybı olmasın */
    for (int i = 0; i < 2; i++) s_cover_buf[i] = heap_caps_malloc(COVER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cover_buf[0] || !s_cover_buf[1]) ESP_LOGW(TAG, "kapak belleği ayrılamadı; kapaklar gösterilmez");
    xTaskCreatePinnedToCore(rx_task, "proto_rx", 4096, NULL, 3, NULL, 0);
    proto_send_hello();
    ESP_LOGI(TAG, "protokol başladı (%d bps)", PROTO_BAUD);
}

/* ---------------- okuyucular ---------------- */
void proto_get(proto_state_t *out)
{
    uint32_t now = proto_now_ms();
    taskENTER_CRITICAL(&s_lock);
    *out = s_state;
    out->alive = s_any_rx && (now - s_last_rx_ms) < PROTO_ALIVE_MS;
    taskEXIT_CRITICAL(&s_lock);
}

int32_t proto_position_ms(const proto_state_t *s)
{
    int64_t pos = s->pos_ms;
    if (s->state == 1) pos += (int64_t)(proto_now_ms() - s->pos_tick_ms);
    if (pos < 0) pos = 0;
    if (s->dur_ms > 0 && pos > s->dur_ms) pos = s->dur_ms;
    return (int32_t)pos;
}

bool proto_local_datetime(proto_datetime_t *o)
{
    taskENTER_CRITICAL(&s_lock);
    bool have = s_have_time;
    int64_t base = s_time_base_ms;
    taskEXIT_CRITICAL(&s_lock);
    if (!have) return false;
    int64_t sec = (base + now64_ms()) / 1000;
    int64_t days = sec / 86400;
    int64_t rem = sec % 86400;
    if (rem < 0) { rem += 86400; days--; }
    o->hh = (int)(rem / 3600);
    o->mm = (int)((rem / 60) % 60);
    o->ss = (int)(rem % 60);
    o->wday = (int)(((days + 3) % 7 + 7) % 7);        /* 1970-01-01 Perşembe */
    /* Gün sayısından takvim tarihi (Howard Hinnant, civil_from_days) */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    o->year = (int)(yoe + era * 400 + (m <= 2));
    o->month = (int)m;
    o->day = (int)d;
    return true;
}

bool proto_weather_get(proto_weather_t *out)
{
    uint32_t now = proto_now_ms();
    taskENTER_CRITICAL(&s_lock);
    *out = s_weather;
    out->valid = s_weather.valid && s_weather_rx_ms != 0 && (now - s_weather_rx_ms) < FRESH_WEATHER_MS;
    taskEXIT_CRITICAL(&s_lock);
    return out->valid;
}

bool proto_sys_get(proto_sys_t *out)
{
    uint32_t now = proto_now_ms();
    taskENTER_CRITICAL(&s_lock);
    *out = s_sys;
    out->valid = s_sys_rx_ms != 0 && (now - s_sys_rx_ms) < FRESH_SYS_MS;
    taskEXIT_CRITICAL(&s_lock);
    return out->valid;
}

bool proto_session_sec(int *sec)
{
    uint32_t now = proto_now_ms();
    taskENTER_CRITICAL(&s_lock);
    uint32_t rx = s_sess_rx_ms;
    int32_t base = s_sess_base_s;
    taskEXIT_CRITICAL(&s_lock);
    if (rx == 0 || (now - rx) >= FRESH_SESSION_MS) return false;
    *sec = base + (int)((now - rx) / 1000);
    return true;
}

void proto_cover_get(proto_cover_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    out->seq = s_cover_seq;
    out->valid = s_cover_valid && s_cover_pub >= 0;
    out->data = out->valid ? s_cover_buf[s_cover_pub] : NULL;
    taskEXIT_CRITICAL(&s_lock);
}

void proto_installer_get(proto_installer_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_inst;
    taskEXIT_CRITICAL(&s_lock);
}

/* ---------------- gönderme ---------------- */
void proto_send_cmd(const char *cmd)
{
    printf("@C\t%s\n", cmd);
    fflush(stdout);
}

void proto_send_seek(int32_t delta_ms)
{
    printf("@C\tseek\t%ld\n", (long)delta_ms);
    fflush(stdout);
}

void proto_send_seek_to(int32_t pos_ms)
{
    printf("@C\tseekto\t%ld\n", (long)pos_ms);
    fflush(stdout);
}

void proto_send_alarm(const char *title, const char *body, bool soft)
{
    printf("@C\talarm\t%s\t%s\t%c\n", title, body, soft ? 'm' : 'z');
    fflush(stdout);
}

void proto_send_hello(void)
{
    printf("@P\tmasa-paneli\t2\t%s\n", FW_VERSION);
    fflush(stdout);
}

void proto_local_set_state(int state)
{
    taskENTER_CRITICAL(&s_lock);
    s_state.pos_ms = proto_position_ms(&s_state);
    s_state.pos_tick_ms = proto_now_ms();
    s_state.state = state;
    taskEXIT_CRITICAL(&s_lock);
}

void proto_local_seek(int32_t delta_ms)
{
    taskENTER_CRITICAL(&s_lock);
    int64_t pos = (int64_t)proto_position_ms(&s_state) + delta_ms;
    if (pos < 0) pos = 0;
    if (s_state.dur_ms > 0 && pos > s_state.dur_ms) pos = s_state.dur_ms;
    s_state.pos_ms = (int32_t)pos;
    s_state.pos_tick_ms = proto_now_ms();
    taskEXIT_CRITICAL(&s_lock);
}

void proto_local_seek_to(int32_t pos_ms)
{
    taskENTER_CRITICAL(&s_lock);
    if (pos_ms < 0) pos_ms = 0;
    if (s_state.dur_ms > 0 && pos_ms > s_state.dur_ms) pos_ms = s_state.dur_ms;
    s_state.pos_ms = pos_ms;
    s_state.pos_tick_ms = proto_now_ms();
    s_seek_hold_ms = s_state.pos_tick_ms + SEEK_HOLD_MS;
    taskEXIT_CRITICAL(&s_lock);
}
