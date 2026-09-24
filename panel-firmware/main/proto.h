#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Panel <-> bilgisayar seri protokolü (USB-UART 921600 bps, satır tabanlı, UTF-8, alan ayıracı TAB).
 * Yalnızca '@' ile başlayan satırlar protokoldür; diğer satırlar (ESP günlükleri) yok sayılır.
 * Bilinmeyen sayısal değer: -999 (PROTO_NA).
 *
 *   PC -> panel
 *     @H                                      kalp atışı (1 sn'de bir)
 *     @S <durum> <konum_ms> <süre_ms> <başlık> <sanatçı>
 *                                             durum: 0 = çalan yok, 1 = çalıyor, 2 = duraklatıldı
 *     @T <yerel_epoch_sn>                     yerel saat (saat dilimi eklenmiş Unix saniyesi): saat ve tarih
 *     @W <hadise> <sıcaklık> <hissedilen> <nem> <en_düşük> <en_yüksek> <gece 0/1> <yer>
 *                                             MGM hava durumu; hadise boşsa veri yok
 *     @M <cpu%> <cpu_sıcaklık> <ram%> <ram_kullanılan_MB> <ram_toplam_MB> <gpu%> <gpu_sıcaklık>
 *        <vram%> <vram_kullanılan_MB> <vram_toplam_MB> <gpu_güç_W> <indirme_kbps> <yükleme_kbps> <gecikme_ms>
 *                                            sistem izleme (1 sn'de bir)
 *     @U <oturum_sn>                          bilgisayar başında kesintisiz geçen süre (5 sn'de bir; 5 dk hareketsizlikte sıfırlanır)
 *     @N <bilgisayar_adı> <işlemci_adı> <ekran_kartı_adı> <iş_parçacığı>    donanım adları
 *     @G <id> <genişlik> <yükseklik> <crc32> <bayt>   kapak başlangıcı (genişlik 0 = kapak yok)
 *     @I <id> <ofset> <base64>                kapak verisi (RGB565, küçük uçlu)
 *     @E <id>                                 kapak sonu; panel CRC32 denetler
 *     @?                                      tanılama isteği
 *     @? shot                                 ekran görüntüsü iste (@F/@f satırlarıyla döner)
 *     @? go <ekran_no>                        tanılama: belirtilen ekrana geç
 *     @? tap <x> <y> [ms]                     tanılama: sanal dokunma (varsayılan 160 ms basılı)
 *     @? mola <sn>                            tanılama: mola hatırlatıcı aralığını geçici olarak <sn> saniye yap (0 = kaldır)
 *     @? saat <dakika> <en_az_oturum_sn>      tanılama: "saat başı" hatırlatmasını HH:<dakika>'da ve verilen en az oturum süresiyle sına
 *     @? drag <x1> <y1> <x2> <y2> <ms> [bekleme_ms]   tanılama: sanal sürükleme
 *     @? perf                                 tanılama: çizim kare/sn ve LVGL meşgul süresini "PERF ..." satırı olarak yaz
 *     @KAL                                    kurulum dosyasını USB-seri üzerinden gönder iste (Wi-Fi'siz bilgisayar için;
 *                                              bkz. pc-helper yok, panel_helper.py değil, PowerShell'in kendisi gönderir)
 *   panel -> PC
 *     @P <ad> <protokol_sürümü> <yazılım_sürümü>   açılışta ve "Yeniden dene"de (yazılım sürümü fw_version.h; eski yazılımda yok)
 *     @X <x> <y>                              tanılama: dokunma başladığında koordinat
 *     @! <seq> <durum> <canlı> <ekran> <boş_heap> <boş_psram> <yazılım_sürümü>   "@?" isteğine yanıt (yazılım sürümü eski yazılımda yok)
 *     @C play_pause | next | prev | seek <fark_ms> | seekto <konum_ms> | cover_retry <id> | alarm <başlık> <gövde> <tür>   komutlar
 *        (alarm türü: z = zamanlayıcı bitti [kalıcı bildirim], m = mola hatırlatıcı [kısa süre görünen bildirim])
 *     @F <genişlik> <yükseklik> | @f <ofset> <base64> | @F end       ekran görüntüsü (RGB565)
 *     @KBAS <toplam_bayt> <sha256_hex>        kurulum dosyası gönderimi başlıyor (ham bayt akışından hemen önce)
 *        Bundan sonra <toplam_bayt> kadar HAM (base64'süz, satır tabanlı olmayan) bayt akışı gelir; PC bunu doğrudan
 *        dosyaya yazar, bitince SHA-256 karşılaştırır. "@KAL" isteğine yalnızca bu şekilde yanıt verilir, başka satır yok.
 */

#define PROTO_BAUD 921600
#define PROTO_NA (-999)
#define PROTO_TEXT_MAX 160
#define PROTO_ALIVE_MS 4000   /* bu süre kalp atışı gelmezse "bağlantı yok" */
#define PROTO_COVER_W 128
#define PROTO_COVER_H 128

typedef struct {
    uint32_t seq;                    /* her @S geldiğinde artar */
    int      state;                  /* 0 yok, 1 çalıyor, 2 duraklatıldı */
    int32_t  pos_ms;                 /* @S'nin geldiği andaki konum */
    int32_t  dur_ms;                 /* 0 = bilinmiyor */
    uint32_t pos_tick_ms;            /* pos_ms'in alındığı tick (esp_timer, ms) */
    char     title[PROTO_TEXT_MAX];
    char     artist[PROTO_TEXT_MAX];
    bool     alive;                  /* son PROTO_ALIVE_MS içinde PC'den bir şey geldi */
} proto_state_t;

typedef struct {
    int year, month, day;            /* month 1..12 */
    int wday;                        /* 0 = Pazartesi .. 6 = Pazar */
    int hh, mm, ss;
} proto_datetime_t;

typedef struct {
    bool valid;                      /* veri var ve taze */
    char code[8];                    /* MGM hadise kodu (A, PB, CB, HY, ...) */
    int  temp, feels, hum, tmin, tmax;
    bool night;
    char place[48];
} proto_weather_t;

typedef struct {
    bool valid;                      /* son 5 sn içinde @M geldi */
    int cpu, cpu_temp, ram, ram_used_mb, ram_total_mb;
    int gpu, gpu_temp, vram, vram_used_mb, vram_total_mb, gpu_power;
    int net_down_kbps, net_up_kbps, ping_ms;
    int threads;
    char host[40], cpu_name[48], gpu_name[48];
} proto_sys_t;

typedef struct {
    uint32_t seq;                    /* kapak değiştikçe artar */
    bool valid;                      /* false = kapak yok */
    const uint8_t *data;             /* RGB565, PROTO_COVER_W x PROTO_COVER_H; sonraki iki kapağa kadar geçerli */
} proto_cover_t;

typedef enum { INSTALLER_IDLE, INSTALLER_SENDING, INSTALLER_DONE } proto_installer_state_t;

typedef struct {
    proto_installer_state_t state;
    uint32_t sent, total;            /* bayt cinsinden; total sabit (gömülü dosya boyutu) */
} proto_installer_t;

void proto_start(void);
void proto_get(proto_state_t *out);
uint32_t proto_now_ms(void);
/* Şu anki tahmini konum (çalıyorsa akıtılmış), süreyi aşmaz */
int32_t proto_position_ms(const proto_state_t *s);
/* Yerel tarih ve saat; false = PC saat bilgisi vermedi */
bool proto_local_datetime(proto_datetime_t *out);
bool proto_weather_get(proto_weather_t *out);
bool proto_sys_get(proto_sys_t *out);
/* Bilgisayar başında geçen kesintisiz süre (sn); false = bilgisayar bilgi vermiyor/bağlı değil */
bool proto_session_sec(int *sec);
void proto_cover_get(proto_cover_t *out);
/* USB-seri üzerinden kurulum dosyası gönderimi (Wi-Fi'siz bilgisayarlar için); "@KAL" geldiğinde kendiliğinden başlar */
void proto_installer_get(proto_installer_t *out);

void proto_send_cmd(const char *cmd);
void proto_send_seek(int32_t delta_ms);
/* Parçayı mutlak konuma (ms) sar; PC'ye "@C seekto <ms>" gider */
void proto_send_seek_to(int32_t pos_ms);
void proto_send_hello(void);
/* Bilgisayardan Windows bildirimi iste (başlık ve gövde Türkçe). soft = mola hatırlatıcı (kısa süre görünür, rahatsız etmesin);
 * değilse zamanlayıcı alarmı (kapatılana kadar kalır). */
void proto_send_alarm(const char *title, const char *body, bool soft);
/* Kullanıcı dokunuşunda arayüz durumunu hemen güncelle (PC yanıtı gelene kadar) */
void proto_local_set_state(int state);
void proto_local_seek(int32_t delta_ms);
/* Mutlak sarma: konumu hemen uygular; PC'nin sarmadan önceki (eski) konum bildirimleri kısa süre yok sayılır ki çubuk geri sıçramasın */
void proto_local_seek_to(int32_t pos_ms);
