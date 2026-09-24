/* Panelde görünen TÜM metinler burada, Türkçe (kullanıcı kuralı: arayüz ve menüler kesinlikle Türkçe).
 * Yeni bir metin gerekirse buraya eklenir; ekranda İngilizce metin bırakılmaz. */
#pragma once

/* Şimdi çalıyor */
#define TR_NOW_PLAYING      "Şimdi çalıyor"
#define TR_PAUSED           "Duraklatıldı"
#define TR_NO_MEDIA_TITLE   "Çalan parça yok"
#define TR_NO_MEDIA_ARTIST  "YouTube Music'te bir şarkı başlat"

/* Ayarlar */
#define TR_SETTINGS         "Ayarlar"
#define TR_BRIGHTNESS       "Parlaklık"
#define TR_IDLE_DIM         "Boşta karart"
#define TR_WAKE_TOUCH       "Dokununca uyanır"
#define TR_DIM_30S          "30 sn"
#define TR_DIM_2M           "2 dk"
#define TR_DIM_10M          "10 dk"
#define TR_DIM_OFF          "Kapalı"

/* Tema */
#define TR_THEME            "Tema"
#define TR_THEME_HINT       "Dokunduğun tema hemen uygulanır."
#define TR_COVER_COLOR      "Kapak rengi"
#define TR_COVER_COLOR_HINT "Vurgu rengi kapaktan gelir"

/* Boşta */
#define TR_TAP_TO_WAKE      "Uyandırmak için dokun"
#define TR_NO_TIME          "--:--"
#define TR_FEELS_LIKE       "Hissedilen"
#define TR_HUMIDITY         "Nem"
#define TR_TEMP_HIGH        "En yüksek"
#define TR_TEMP_LOW         "En düşük"

/* Sistem izleme */
#define TR_SYSTEM           "Sistem"
#define TR_CPU              "İşlemci"
#define TR_GPU              "Ekran kartı"
#define TR_RAM              "Bellek"
#define TR_VRAM             "Video belleği"
#define TR_THREADS          "iş parçacığı"
#define TR_NOT_FOUND        "Algılanmadı"
#define TR_DOWNLOAD         "İndirme"
#define TR_UPLOAD           "Yükleme"
#define TR_LATENCY          "Gecikme"
#define TR_SESSION          "Oturum"
#define TR_USED_TOTAL       "Kullanılan / toplam"
#define TR_WAIT_DATA        "Veri bekleniyor…"

/* Zamanlayıcı */
#define TR_TIMER            "Zamanlayıcı"
#define TR_COUNTDOWN        "Geri sayım"
#define TR_POMODORO         "Pomodoro"
#define TR_START            "Başlat"
#define TR_PAUSE            "Duraklat"
#define TR_RESUME           "Sürdür"
#define TR_RESET            "Sıfırla"
#define TR_READY            "Hazır"
#define TR_RUNNING_NOW      "Çalışıyor"
#define TR_TOTAL_TIME       "Toplam süre"
#define TR_ENDS_AT          "Bitiş saati"
#define TR_FOCUS            "Odak"
#define TR_SHORT_BREAK      "Kısa mola"
#define TR_LONG_BREAK       "Uzun mola"
#define TR_ROUND_OF_4       "Tur"
#define TR_POMO_PLAN        "25 dk odak · 5 dk mola · 4 turda bir 15 dk"
#define TR_NEXT             "Sıradaki"
#define TR_TIME_UP          "Süre doldu"
#define TR_FOCUS_DONE       "Odak bitti"
#define TR_BREAK_DONE       "Mola bitti"
#define TR_START_BREAK      "Molayı başlat"
#define TR_START_FOCUS      "Odağı başlat"
#define TR_DISMISS          "Kapat"

/* Mola hatırlatıcı */
#define TR_BREAK            "Mola"
#define TR_SESSION_TIME     "Oturum süresi"
#define TR_NEXT_BREAK       "Sıradaki mola"
#define TR_OFF              "Kapalı"
#define TR_TOOK_BREAK       "Mola verdim"
#define TR_BREAK_TIME       "Mola zamanı"
#define TR_HOURLY           "Saat başı"

/* Bağlantı yok */
#define TR_WAITING_PC       "Bilgisayar bekleniyor"
#define TR_WAITING_HELP     "Program bu bilgisayarda kurulu değilse aşağıdan kur; kuruluysa açık olsun."
#define TR_INSTALL_BTN      "Bu bilgisayara kur"
#define TR_INSTALL_S1       "Bilgisayarın Wi-Fi listesinden bu ağa bağlan"
#define TR_INSTALL_S2       "Şifre"
#define TR_INSTALL_S3       "Açılan sayfadan dosyayı indir, kendi Wi-Fi ağına dönüp çalıştır. Sayfa açılmazsa tarayıcıya 192.168.4.1 yaz."
#define TR_INSTALL_QR       "Kamerayla oku"
#define TR_INSTALL_STARTING "Wi-Fi hazırlanıyor…"
#define TR_INSTALL_READY    "Hazır · bağlı cihaz: %d · indirilen: %d"
#define TR_INSTALL_FAIL     "Wi-Fi başlatılamadı"
#define TR_CLOSE            "Kapat"
#define TR_PANEL            "Panel"
#define TR_RUNNING          "Çalışıyor"
#define TR_PC_APP           "Bilgisayar uygulaması"
#define TR_WAIT_REPLY       "Yanıt bekleniyor"
#define TR_SEARCHING        "Aranıyor…"
#define TR_RETRY            "Yeniden dene"
#define TR_TRYING           "Deneniyor…"

/* Kurulum yöntemi seçimi + USB ile kurulum */
#define TR_INSTALL_CHOOSE_TITLE "Nasıl kurulsun?"
#define TR_INSTALL_WIFI_TITLE   "Wi-Fi ile"
#define TR_INSTALL_WIFI_SUB     "Bilgisayarın kablosuz ağı varsa"
#define TR_INSTALL_USB_TITLE    "USB ile"
#define TR_INSTALL_USB_SUB      "Wi-Fi'si olmayan (yalnızca Ethernet'li) bilgisayarlar için"
#define TR_INSTALL_USB_HINT     "Panelin USB kablosuyla bağlı olduğu bilgisayarda PowerShell'i aç, aşağıdaki satırı yaz ve Enter'a bas (internet gerekir)."
#define TR_INSTALL_USB_WAITING  "Bekleniyor…"
#define TR_INSTALL_USB_SENDING  "Gönderiliyor · %%%d"
#define TR_INSTALL_USB_DONE     "Tamamlandı · bilgisayarda kurulum kendiliğinden başlayacak"
/* PowerShell'e yazılacak/yapıştırılacak TEK satır; gerçek USB-seri alım mantığı ayrıca yayınlanan küçük bir script'te
 * (bkz. pc-helper/bootstrap/masapaneli-kur.ps1 — GitHub Gist'e elle yayınlandı, panel derleme zamanında sadece bu tek
 * satırı gömer). Gist güncellenirse (aynı URL, en güncel sürümü döndürür) panel yazılımını yeniden derlemeye gerek yok. */
#define TR_INSTALL_USB_SCRIPT \
    "irm https://gist.githubusercontent.com/gpplay6521-cmd/79484443670b2ec5832c3813c982e733/raw/masapaneli-kur.ps1 | iex"
