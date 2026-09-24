"""Masa paneli yardımcı programı: panelin USB seri portundan bilgisayarla konuşan köprü.

  - Windows medya oturumunu (SMTC): çalan parça, süre/konum, kapak resmi -> panele; panelden gelen komutlar
    (oynat/duraklat, önceki, sonraki, ±10 sn) -> medya oturumuna
  - Saat ve tarih -> panele
  - Hava durumu (Meteoroloji Genel Müdürlüğü) -> panele; konum bilgisayardan bulunur (bulunamazsa konum.py'daki varsayılan), panel_ayarlari.json ile elle de verilebilir
  - Zamanlayıcı bitince (panel bildirir) Windows bildirimi
  - Sistem izleme (işlemci, bellek, ekran kartı; bu bilgisayarınki) -> panele

Protokol satırları (UTF-8, TAB ayraçlı, '@' ile başlar) panel yazılımındaki proto.h ile aynıdır.

Kullanım:
  .venv\\Scripts\\python -X utf8 panel_helper.py                 # paneli otomatik bulur (CH340)
  .venv\\Scripts\\python -X utf8 panel_helper.py --dry-run       # seri port açmaz, gönderilecek satırları yazar
  .venv\\Scripts\\python -X utf8 panel_helper.py --il Ankara --ilce Çankaya
  .venv\\Scripts\\python -X utf8 panel_helper.py --script "go system;shot sistem" --exit-after-script   # ekran görüntüsü tanılaması
"""
import argparse
import asyncio
import base64
import ctypes
import json
import os
import sys
import threading
import time
import traceback
import unicodedata
import zlib
from datetime import datetime, timezone
from pathlib import Path

import serial
import websockets
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as SessionManager,
    GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
)

import konum
import notify
import cover
import oturum
import panel_yukle
import panelport
import sysmon
import weather

HERE = Path(__file__).parent
CONFIG_FILE = HERE / "panel_ayarlari.json"
KAPAK_KAYIT = HERE / "kapak_son"
BAUD = panelport.BAUD                                 # panel yazılımındaki PROTO_BAUD ile aynı
DEFAULT_SOURCE = "crx_cinhimbnkkghhklpknlkffjgod"   # Chrome'da kurulu YouTube Music uygulaması (bu bilgisayarda gözlendi)
TEXT_MAX_BYTES = 150                                  # panel tarafındaki PROTO_TEXT_MAX (160) altında
NA = -999
SCREEN_IDS = {"main": 0, "settings": 1, "theme": 2, "idle": 3, "offline": 4, "system": 5, "timer": 6, "alarm": 7,
              "install": 8, "install_choose": 9, "install_usb": 10}
SCREEN_NAMES_TR = ["Şimdi çalıyor", "Ayarlar", "Tema", "Boşta", "Bağlantı yok", "Sistem", "Zamanlayıcı", "Alarm", "Kurulum"]


LOG_FILE = HERE / "panel_helper_gunluk.txt"
_MUTEX = None


def setup_logging():
    """Konsol yoksa (pythonw ile, örn. Windows açılışında otomatik başlatma) ya da --gunluk verildiyse çıktıyı günlük dosyasına yönlendirir.
    Dosya 1 MB'ı geçince başlangıçta panel_helper_gunluk.eski.txt'e taşınır (en çok iki dosya kalır)."""
    if sys.stdout is not None and sys.stderr is not None and not ARGS.gunluk:
        return
    try:
        if LOG_FILE.exists() and LOG_FILE.stat().st_size > 1_000_000:
            LOG_FILE.replace(LOG_FILE.with_name("panel_helper_gunluk.eski.txt"))
        f = open(LOG_FILE, "a", encoding="utf-8", buffering=1)
    except OSError:
        f = open(os.devnull, "w")
    sys.stdout = sys.stderr = f


def single_instance():
    """Aynı anda tek yardımcı çalışsın: iki kopya aynı seri portu kapışır. Windows adlandırılmış mutex; False = zaten çalışıyor."""
    global _MUTEX
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateMutexW.restype = ctypes.c_void_p
    k32.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p]
    _MUTEX = k32.CreateMutexW(None, False, "Local\\MasaPaneliYardimci")
    return ctypes.get_last_error() != 183          # 183 = ERROR_ALREADY_EXISTS


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def clean_text(text, limit=TEXT_MAX_BYTES):
    """Sekme/satır sonu/denetim karakterlerini temizle, UTF-8 karakter sınırında kes.
    Unicode NFC: bazı parça adları harfi "u" + ayrı birleşik ünlem işareti (U+0308) olarak verir ("Bu\\u0308klu\\u0308m");
    panelin yazı tipleri birleşik işaretleri çizemez (kutu çıkar), NFC bunları tek karaktere ("ü") çevirir.
    Birleşmeyen artık Latin birleşik işaretleri (U+0300–036F) atılır: kutu çizmektense hiç çizilmesin."""
    text = unicodedata.normalize("NFC", text or "")
    text = "".join(c for c in text if not 0x300 <= ord(c) <= 0x36F)
    text = "".join(" " if (c in "\t\r\n" or ord(c) < 32) else c for c in text).strip()
    raw = text.encode("utf-8")
    if len(raw) <= limit:
        return text
    return raw[:limit].decode("utf-8", "ignore")


def find_port():
    """Panelin (CH340K USB-seri) portu. Bilgisayarda tek CH340 aygıtı varsa doğrudan o; birden çoksa (örn. bir Arduino klonu da
    takılıysa) hangisinin panel olduğu yoklanarak bulunur (panelport.py)."""
    cands = panelport.candidates()
    if len(cands) <= 1:
        return cands[0] if cands else None
    return panelport.find_panel()[0]


def load_config():
    """panel_ayarlari.json: {"il": "", "ilce": "", "otomatik_konum": true, "ping_hedef": "1.1.1.1"}; komut satırı bunun üzerine yazar.
    il boşsa konum bilgisayardan bulunur (bulunamazsa konum.py'daki varsayılan); il yazılırsa o kullanılır.
    ping_hedef: gecikme ölçümü için ulaşılacak sunucu (TCP 443); oyun sunucusunun adresi yazılabilir.
    medya_kaynagi: kontrol edilecek medya uygulamasının kimliğinde aranacak metin (varsayılan: Chrome'daki YouTube Music uygulaması;
    bulunamazsa ya da boşsa Windows'un "şu an çalan" oturumu, yani Spotify, tarayıcı vb. herhangi bir oynatıcı kullanılır).
    otomatik_yazilim_guncelle: panel bağlanınca yazılım sürümü bu paketteki (firmware\\) sürümden eskiyse yeni yazılımı panele kendiliğinden yükle
    (panel_yukle.py; yaklaşık 1 dk, o sırada panel ekranı çalışmaz). Yalnızca kendini Masa Paneli olarak tanıtan panellere dokunulur."""
    cfg = {"il": "", "ilce": "", "otomatik_konum": True, "ping_hedef": "1.1.1.1", "medya_kaynagi": DEFAULT_SOURCE,
           "otomatik_yazilim_guncelle": True}
    try:
        cfg.update({k: v for k, v in json.loads(CONFIG_FILE.read_text(encoding="utf-8")).items() if k in cfg})
    except FileNotFoundError:
        pass
    except Exception as e:  # noqa: BLE001
        log(f"{CONFIG_FILE.name} okunamadı: {e}")
    if ARGS.il is not None:
        cfg["il"] = ARGS.il
    if ARGS.ilce is not None:
        cfg["ilce"] = ARGS.ilce
    return cfg


class Panel:
    """Seri bağlantı; yazma kilitli, okuma ayrı iş parçacığında."""

    def __init__(self, port, baud, dry_run):
        self.port, self.baud, self.dry_run = port, baud, dry_run
        self.used_port = None      # son açılan port (panel yazılımı güncellemesi için)
        self.ser = None
        self.lock = threading.Lock()
        self.rx_lines = None  # asyncio.Queue
        self.loop = None

    def open(self):
        if self.dry_run:
            return True
        port = self.port or find_port()
        if not port:
            return False
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, self.baud, 0.2
        s.dtr = False          # kartı sıfırlayan DTR/RTS hatlarını mümkün olduğunca boşta tut
        s.rts = False
        s.open()
        self.ser = s
        self.used_port = port
        threading.Thread(target=self._reader, daemon=True).start()
        log(f"Panele bağlanıldı: {port} ({self.baud} bps)")
        return True

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:  # noqa: BLE001
                pass
            self.ser = None

    def _reader(self):
        buf = b""
        ser = self.ser
        while ser is not None and ser is self.ser:
            try:
                data = ser.read(4096)
            except Exception:  # noqa: BLE001
                break
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                self.loop.call_soon_threadsafe(self.rx_lines.put_nowait, text)
        if ser is self.ser:
            self.loop.call_soon_threadsafe(self.rx_lines.put_nowait, None)   # bağlantı koptu

    def send(self, *fields):
        line = "\t".join(str(f) for f in fields)
        if self.dry_run:
            if fields[0] != "@I":                       # kapak veri satırları çok: yalnızca başı ve sonu yazılır
                print("  PANELE ->", (line if len(line) < 140 else line[:140] + "…").replace("\t", " | "), flush=True)
            return True
        try:
            with self.lock:
                self.ser.write((line + "\n").encode("utf-8"))
            return True
        except Exception:  # noqa: BLE001
            return False


class Media:
    """Windows medya oturumu: durum okuma, kapak ve komutlar."""

    def __init__(self, source):
        self.source = (source or "").lower()
        self.mgr = None

    async def start(self):
        self.mgr = await SessionManager.request_async()

    def session(self):
        sessions = list(self.mgr.get_sessions())
        for s in sessions:
            if self.source and self.source in (s.source_app_user_model_id or "").lower():
                return s
        return self.mgr.get_current_session()

    @staticmethod
    def position_ms(s):
        """Zaman çizelgesi birkaç sn'de bir güncellenir; konum 'son güncelleme + geçen süre' ile hesaplanır."""
        tl = s.get_timeline_properties()
        pos = tl.position.total_seconds()
        dur = tl.end_time.total_seconds() - tl.start_time.total_seconds()
        if s.get_playback_info().playback_status == PlaybackStatus.PLAYING:
            pos += (datetime.now(timezone.utc) - tl.last_updated_time).total_seconds()
        pos = max(0.0, min(pos, dur) if dur > 0 else pos)
        return int(pos * 1000), int(max(dur, 0) * 1000)

    async def snapshot(self):
        s = self.session()
        if s is None:
            return (0, 0, 0, "", "")
        props = await s.try_get_media_properties_async()
        status = s.get_playback_info().playback_status
        title, artist = clean_text(props.title), clean_text(props.artist)
        if not title and not artist:
            return (0, 0, 0, "", "")
        state = 1 if status == PlaybackStatus.PLAYING else 2
        pos, dur = self.position_ms(s)
        return (state, pos, dur, title, artist)

    async def thumbnail(self):
        """Çalan parçanın küçük resmi (ham baytlar) ya da None."""
        s = self.session()
        if s is None:
            return None
        return await cover.read_thumbnail(await s.try_get_media_properties_async())

    async def command(self, name, args):
        s = self.session()
        if s is None:
            log(f"Komut '{name}' atlandı: çalan medya oturumu yok")
            return
        if name == "play_pause":
            ok = await s.try_toggle_play_pause_async()
        elif name == "next":
            ok = await s.try_skip_next_async()
        elif name == "prev":
            ok = await s.try_skip_previous_async()
        elif name == "seek" and args:
            pos, dur = self.position_ms(s)
            target = max(0, pos + int(args[0]))
            if dur > 0:
                target = min(target, max(dur - 500, 0))
            ok = await s.try_change_playback_position_async(target * 10_000)   # 100 ns birimi
        elif name == "seekto" and args:                       # mutlak konum (panelde topu sürükleyerek sarma)
            _, dur = self.position_ms(s)
            target = max(0, int(args[0]))
            if dur > 0:
                target = min(target, max(dur - 500, 0))
            ok = await s.try_change_playback_position_async(target * 10_000)
        else:
            log(f"Bilinmeyen komut: {name} {args}")
            return
        log(f"Komut {'uygulandı' if ok else 'REDDEDİLDİ (uygulama kabul etmedi)'}: {name} {' '.join(args)}".rstrip())


class ExtBridge:
    """YouTube Music sayfasındaki tarayıcı eklentisiyle yerel WebSocket köprüsü (beğen/karıştır/tekrarla).
    SMTC (Windows medya kontrolü) bu üç eylemi desteklemediği için sayfaya doğrudan eklenti üzerinden ulaşılır
    (bkz. chrome-eklenti/). Eklenti bu sunucuya bağlanır, biz de panelden gelen komutu ona iletiriz."""

    def __init__(self):
        self.clients = set()

    async def handler(self, ws):
        self.clients.add(ws)
        log("YouTube Music eklentisi bağlandı")
        try:
            async for _ in ws:
                pass
        finally:
            self.clients.discard(ws)
            log("YouTube Music eklentisi bağlantısı kesildi")

    async def send(self, cmd):
        if not self.clients:
            log(f"Kısayol '{cmd}' gönderilemedi: YouTube Music eklentisi bağlı değil "
                f"(Chrome'da chrome-eklenti yüklü ve music.youtube.com açık mı?)")
            return
        msg = json.dumps({"cmd": cmd})
        dead = []
        for ws in list(self.clients):
            try:
                await ws.send(msg)
            except Exception:  # noqa: BLE001
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)
        log(f"Kısayol eklentiye iletildi: {cmd}")


async def ext_bridge_loop(sess, stop):
    try:
        async with websockets.serve(sess.ext_bridge.handler, "127.0.0.1", 8934):
            await stop.wait()
    except OSError as e:
        log(f"Eklenti köprüsü (WebSocket :8934) başlatılamadı: {e}")


class Session:
    """Bir seri bağlantı süresince tutulan durum (bağlantı yenilenince sıfırlanır)."""

    def __init__(self, panel, media):
        self.panel, self.media = panel, media
        self.ext_bridge = ExtBridge()
        self.cur_key = None            # şu an çalan parçanın (başlık, sanatçı) anahtarı
        self.cover_id = 0
        self.cover_data = None         # gönderilen son kapak (yeniden deneme için)
        self.cover_retries = 0
        self.cover_sent = None         # panelde görünen resmin özeti: None = bilinmiyor, 0 = yer tutucu (kapak yok), sayı = CRC32
        self.cover_reset = False       # true: izleyici panelde ne olduğunu bilmiyor sayıp kapağı baştan gönderir
        self.shot = None               # devam eden ekran görüntüsü
        self.shot_done = asyncio.Event()
        self.shot_path = None
        self.weather = None            # son hava durumu (sözlük) ve alındığı zaman
        self.weather_at = 0.0
        self.fw_version = None         # panelin bildirdiği yazılım sürümü: None = henüz bilinmiyor, "" = sürüm bildirmeyen eski yazılım

    def send_frames(self, frames):
        """Kapak satırlarını sırayla gönder (ayrı iş parçacığında çalıştırılır; kalp atışı satırları araya girebilir)."""
        for f in frames:
            if not self.panel.send(*f):
                return False
        return True


COVER_POLL_S = 0.5      # küçük resim bu aralıkla izlenir
COVER_SETTLE_S = 1.5    # yeni bir resim panele gitmeden önce bu süre değişmeden durmalı (anlık/geçici resimler elenir)


async def cover_watch(sess, stop):
    """Kapak izleyici: panelde YALNIZCA şarkının kapağı görünsün diye Windows'un verdiği küçük resmi sürekli izler.

    Windows/Chrome parça değişince kısa süre önceki parçanın resmini ya da genel bir simge (örn. Google logosu) verebilir,
    gerçek kapağı sonradan yükler. Bu yüzden: (1) resim en az COVER_SETTLE_S saniye değişmeden durmadan panele gönderilmez,
    (2) gönderildikten sonra da izlenir, resim sonradan değişirse (gerçek kapak geldiyse) güncellenir,
    (3) parça değişince ve yeni resim panelde görünenden farklıysa eski kapak hemen kaldırılır (yer tutucu), yenisi durulunca gelir;
        aynı resimse (aynı albüm) eski kapak kesintisiz kalır."""
    loop = asyncio.get_running_loop()
    last_key = None
    cand, cand_since = None, 0.0
    while not stop.is_set():
        await asyncio.sleep(COVER_POLL_S)
        key = sess.cur_key
        if sess.cover_reset:                           # panel yeniden başladı: her şeyi baştan gönder
            sess.cover_reset = False
            sess.cover_sent = None
            last_key, cand = None, None
        if key is None:                                # çalan parça yok (gönderen taraf kapağı zaten kaldırdı)
            last_key, cand = None, None
            continue
        try:
            data = await sess.media.thumbnail()
        except Exception as e:  # noqa: BLE001
            if ARGS.verbose:
                log(f"Küçük resim okunamadı: {e}")
            continue
        h = zlib.crc32(data) if data else 0
        now = time.time()
        if key != last_key:                            # parça değişti
            last_key, cand = key, None
            if sess.cover_sent not in (None, 0) and h != sess.cover_sent:
                sess.cover_id += 1
                sess.cover_data = None
                sess.panel.send(*cover.clear_frame(sess.cover_id))    # eski kapak yeni parçaya ait değil: kaldır
                sess.cover_sent = 0
        if h != cand:                                  # resim değişti: durulmasını bekle
            cand, cand_since = h, now
            continue
        if now - cand_since < COVER_SETTLE_S or h == sess.cover_sent:
            continue
        try:
            sess.cover_id += 1
            sess.cover_retries = 0
            if not data:
                sess.cover_data = None
                sess.panel.send(*cover.clear_frame(sess.cover_id))
                sess.cover_sent = 0
                continue
            t0 = time.time()
            rendered = await loop.run_in_executor(None, cover.render_565, data)
            if sess.cur_key != key:                    # dönüştürürken parça değişti: bu resmi gönderme
                continue
            sess.cover_data = rendered
            ok = await loop.run_in_executor(None, sess.send_frames, cover.frames(rendered, sess.cover_id))
            sess.cover_sent = h if ok else None
            slot = f"{sess.cover_id % 5}.png"           # son 5 kapak kayıtlı kalır (yanlış resim görülürse incelemek için)
            try:
                KAPAK_KAYIT.mkdir(exist_ok=True)
                w, hh = await loop.run_in_executor(None, cover.save_png, data, KAPAK_KAYIT / slot)
            except Exception:  # noqa: BLE001
                w = hh = 0
            log(f"Kapak gönderildi: {key[0]} — {key[1]} ({w}x{hh}, crc {h:08x}, {time.time() - t0:.2f} sn, ok={ok}, kayıt: kapak_son/{slot})")
        except asyncio.CancelledError:
            raise
        except Exception as e:  # noqa: BLE001
            log(f"Kapak hazırlanamadı: {e}")


async def cover_resend(sess, cid):
    """Panel CRC hatası bildirdi: aynı kapağı yeniden gönder (en çok 2 kez)."""
    if cid != sess.cover_id or not sess.cover_data or sess.cover_retries >= 2:
        return
    sess.cover_retries += 1
    log(f"Kapak yeniden gönderiliyor (#{cid}, deneme {sess.cover_retries})")
    loop = asyncio.get_running_loop()
    await loop.run_in_executor(None, sess.send_frames, cover.frames(sess.cover_data, cid))


def local_epoch():
    """Yerel saat (saat dilimi eklenmiş Unix saniyesi): panel saat ve tarihi bundan türetir."""
    return int(time.time()) + time.localtime().tm_gmtoff


async def sender(sess, stop):
    """Kalp atışı (1 sn), saat (30 sn) ve medya durumu (değişince, en az 2 sn'de bir)."""
    panel, media = sess.panel, sess.media
    last_t = 0.0
    last_hb = 0.0
    last_diag = 0.0
    last_state = None
    last_sent = 0.0
    last_pos_sent = 0
    while not stop.is_set():
        now = time.time()
        if now - last_hb >= 1.0:
            if not panel.send("@H"):
                return
            last_hb = now
        if ARGS.diag and now - last_diag >= 5.0:
            panel.send("@?")
            last_diag = now
        if now - last_t >= 30.0:
            panel.send("@T", local_epoch())
            last_t = now
        try:
            snap = await media.snapshot()
        except Exception as e:  # noqa: BLE001
            log(f"Medya okunamadı: {e}")
            snap = (0, 0, 0, "", "")
        state, pos, dur, title, artist = snap
        key = (state, dur // 1000, title, artist)
        expected = last_pos_sent + int((now - last_sent) * 1000) if last_state and last_state[0] == 1 else last_pos_sent
        jumped = abs(pos - expected) > 1500 and state != 0
        if key != last_state or now - last_sent >= 2.0 or jumped:
            if not panel.send("@S", state, pos, dur, title, artist):
                return
            last_state, last_sent, last_pos_sent = key, now, pos
        track = (title, artist) if state != 0 else None
        if track != sess.cur_key:                    # parça değişti (kapağı cover_watch yönetir); çalan parça kalmadıysa kapağı kaldır
            sess.cur_key = track
            if track is None and sess.cover_sent != 0:
                sess.cover_id += 1
                sess.cover_data = None
                panel.send(*cover.clear_frame(sess.cover_id))
                sess.cover_sent = 0
        await asyncio.sleep(0.4)


async def ping_loop(mon, stop, target):
    """İnternet gecikmesi: 2 sn'de bir TCP bağlantı süresi ölçülür (sonuç mon.ping_ms)."""
    loop = asyncio.get_running_loop()
    while not stop.is_set():
        await loop.run_in_executor(None, mon.measure_ping, target)
        await asyncio.sleep(2.0)


async def session_loop(sess, stop):
    """Oturum süresi (klavye/fare hareketsizliği 5 dk'yı geçince sıfırlanır): 5 sn'de bir panele."""
    tracker = oturum.Oturum()
    loop = asyncio.get_running_loop()
    while not stop.is_set():
        try:
            sec = await loop.run_in_executor(None, tracker.seconds)
            if not sess.panel.send("@U", sec):
                return
        except Exception as e:  # noqa: BLE001
            log(f"Oturum süresi okunamadı: {e}")
        await asyncio.sleep(5.0)


async def sysmon_loop(sess, stop, cfg):
    """Sistem izleme: donanım adları 30 sn'de bir, ölçümler 1 sn'de bir."""
    loop = asyncio.get_running_loop()
    try:
        mon = await loop.run_in_executor(None, sysmon.SysMon, ARGS.gpu)
    except Exception as e:  # noqa: BLE001
        log(f"Sistem izleme başlatılamadı: {e}")
        return
    log("Sistem izleme: " + mon.describe() + f", gecikme hedefi={cfg['ping_hedef']}")
    asyncio.create_task(ping_loop(mon, stop, cfg["ping_hedef"]))
    last_names = 0.0
    while not stop.is_set():
        now = time.time()
        if now - last_names >= 30.0:
            sess.panel.send("@N", clean_text(mon.host, 38), clean_text(mon.cpu_name, 46),
                            clean_text(mon.gpu_name, 46), mon.threads)
            last_names = now
        try:
            vals = await loop.run_in_executor(None, mon.sample)
            if not sess.panel.send("@M", *vals):
                return
        except Exception as e:  # noqa: BLE001
            log(f"Sistem ölçümü okunamadı: {e}")
        await asyncio.sleep(1.0)


async def weather_loop(sess, stop, cfg):
    """MGM hava durumu: 10 dk'da bir alınır, panele 30 sn'de bir (taze kalsın diye) gönderilir."""
    loop = asyncio.get_running_loop()
    loc = None
    want = None                 # (il, ilce) istenen konum
    last_detect = 0.0
    detect_every = 6 * 3600
    last_fetch = 0.0
    warned = False
    while not stop.is_set():
        now = time.time()
        try:
            # Konum: ayar dosyası/komut satırı > bilgisayardan bulma (6 saatte bir yenilenir; panel başka bilgisayara takılabilir) > varsayılan
            if cfg["il"]:
                new_want = (cfg["il"], cfg["ilce"])
            elif not cfg["otomatik_konum"]:
                new_want = (konum.DEFAULT_IL, konum.DEFAULT_ILCE)
            elif want is None or now - last_detect >= detect_every:
                last_detect = now
                il, ilce, why, sure = await loop.run_in_executor(None, konum.auto)
                # Ağ/servis hatasıyla varsayılana düşüldüyse (bilgisayar yeni açılmış, ağ hazır değil) 1 dk sonra yeniden dene
                detect_every = 6 * 3600 if sure else 60
                new_want = (il, ilce)
                if new_want != want:
                    log(f"Konum belirlendi: {il}{', ' + ilce if ilce else ''} — {why}")
            else:
                new_want = want
            if new_want != want or loc is None:
                loc = await loop.run_in_executor(None, weather.resolve, *new_want)
                want = new_want
                sess.weather, last_fetch = None, 0.0
                log(f"Hava durumu konumu: {loc['place']} (MGM merkez {loc['merkez_id']})")
            if now - last_fetch >= 600 or sess.weather is None and now - last_fetch >= 30:
                last_fetch = now
                sess.weather = await loop.run_in_executor(None, weather.fetch, loc)
                sess.weather_at = now
                warned = False
                w = sess.weather
                log(f"Hava durumu: {w['place']} {w['temp']}° {w['code']} (en düşük {w['tmin']}°, en yüksek {w['tmax']}°)")
        except ValueError as e:
            if not warned:
                log(f"Hava durumu konumu bulunamadı: {e}")
                warned = True
            last_fetch = now
        except Exception as e:  # noqa: BLE001
            if not warned:
                log(f"Hava durumu alınamadı (ağ?): {e}")
                warned = True
        w = sess.weather if sess.weather and time.time() - sess.weather_at < 90 * 60 else None
        if w:
            sess.panel.send("@W", w["code"], w["temp"], w["feels"], w["hum"], w["tmin"], w["tmax"],
                            1 if w["night"] else 0, clean_text(w["place"], 46))
        else:
            sess.panel.send("@W", "", NA, NA, NA, NA, NA, 0, "")
        await asyncio.sleep(30.0 if loc else 60.0)


def finish_shot(sess):
    s = sess.shot
    sess.shot = None
    if not s or s["got"] < len(s["buf"]):
        log(f"Ekran görüntüsü eksik geldi ({s['got'] if s else 0} bayt)")
        sess.shot_path = None
    else:
        img = cover.rgb565_to_image(bytes(s["buf"]), s["w"], s["h"])
        Path(sess.shot_path).parent.mkdir(parents=True, exist_ok=True)
        img.save(sess.shot_path)
    sess.shot_done.set()


async def receiver(sess, stop):
    """Panelden gelen satırlar: '@C' komutlar, '@P' merhaba, '@F'/'@f' ekran görüntüsü; diğerleri ESP günlüğü."""
    panel, media = sess.panel, sess.media
    while not stop.is_set():
        line = await panel.rx_lines.get()
        if line is None:
            log("Panel bağlantısı koptu")
            return
        if line.startswith("@C\t"):
            parts = line.split("\t")[1:]
            if parts[0] == "cover_retry" and len(parts) > 1 and parts[1].lstrip("-").isdigit():
                asyncio.create_task(cover_resend(sess, int(parts[1])))
            elif parts[0] == "alarm" and len(parts) >= 3:      # zamanlayıcı bitti / mola zamanı: bilgisayarda bildirim göster
                soft = len(parts) >= 4 and parts[3] == "m"      # m = mola hatırlatıcı (kısa süre görünür), z = zamanlayıcı (kalıcı)
                try:
                    state = await asyncio.get_running_loop().run_in_executor(None, notify.show, parts[1], parts[2], not soft)
                    log(f"{'Mola' if soft else 'Alarm'} bildirimi gönderildi: {parts[1]} — {parts[2]} (bildirim ayarı: {state})")
                except Exception as e:  # noqa: BLE001
                    log(f"Alarm bildirimi gösterilemedi: {e}")
            elif parts[0] in ("like", "shuffle", "repeat"):
                # SMTC (Windows medya kontrolü) bunları desteklemiyor; YouTube Music sayfasına
                # tarayıcı eklentisiyle iletilir (bkz. ExtBridge, chrome-eklenti/).
                await sess.ext_bridge.send(parts[0])
            else:
                await media.command(parts[0], parts[1:])
        elif line.startswith("@P"):
            f = line.split("\t")
            sess.fw_version = f[3].strip() if len(f) >= 4 else ""      # "@P masa-paneli <protokol> <yazılım sürümü>"; eski yazılımda sürüm yok
            log("Panel merhaba dedi; durum yeniden gönderiliyor")
            panel.send("@T", local_epoch())
            sess.cover_reset = True                  # kapak dahil her şey yeniden gönderilsin
        elif line.startswith("@!\t"):
            p = line.split("\t")[1:]
            if len(p) >= 6:
                sess.fw_version = p[6].strip() if len(p) >= 7 else ""
            if len(p) >= 6 and (ARGS.diag or ARGS.verbose):
                scr = SCREEN_NAMES_TR[int(p[3])] if p[3].isdigit() and int(p[3]) < len(SCREEN_NAMES_TR) else p[3]
                log(f"Panel durumu: ekran={scr}, PC canlı={'evet' if p[2] == '1' else 'hayır'}, "
                    f"alınan durum={p[1]}, boş bellek={int(p[4]) // 1024} KB, boş PSRAM={int(p[5]) // 1024} KB")
        elif line.startswith("@X\t"):
            p = line.split("\t")[1:]
            if ARGS.diag or ARGS.verbose:
                log(f"Panel dokunma algıladı: x={p[0]}, y={p[1]}")
        elif line.startswith("@F\t"):
            p = line.split("\t")[1:]
            if p[0] == "end":
                finish_shot(sess)
            elif len(p) >= 2:
                w, h = int(p[0]), int(p[1])
                sess.shot = {"w": w, "h": h, "buf": bytearray(w * h * 2), "got": 0}
        elif line.startswith("@f\t") and sess.shot:
            p = line.split("\t")
            try:
                off, raw = int(p[1]), base64.b64decode(p[2])
                sess.shot["buf"][off:off + len(raw)] = raw
                sess.shot["got"] += len(raw)
            except Exception:  # noqa: BLE001
                pass
        elif line.startswith("PERF"):
            log("Panel " + line)
        elif line.strip() and ARGS.verbose:
            print("  panel günlüğü:", line, flush=True)


async def script_runner(sess, stop):
    """Tanılama: panele sanal dokunma gönderip ekranlar arasında gezer, çerçeve tamponunu PNG olarak kaydeder.
    Adımlar ';' ile ayrılır:  go <ekran> | tap <x> <y> | tapq <x> <y> | wait <sn> | mola <sn> | saat <dk> <oturum_sn> | shot <dosya_adı>
    Örnek: --script "go timer;shot zaman_bos;tap 584 300;wait 1;shot zaman_calisiyor"."""
    await asyncio.sleep(ARGS.script_delay)
    outdir = Path(ARGS.script_dir)
    for step in ARGS.script.split(";"):
        p = step.strip().split()
        if not p:
            continue
        if p[0] == "go" and len(p) == 2 and p[1] in SCREEN_IDS:
            sess.panel.send("@?", "go", SCREEN_IDS[p[1]])
            await asyncio.sleep(1.4)
        elif p[0] in ("tap", "tapq") and len(p) == 3:      # tapq: gerçek parmak gibi kısa (60 ms) basış, 0,2 sn arayla art arda denemek için
            sess.panel.send("@?", "tap", p[1], p[2], *(() if p[0] == "tap" else (60,)))
            await asyncio.sleep(0.9 if p[0] == "tap" else 0.2)
        elif p[0] == "wait" and len(p) == 2:
            await asyncio.sleep(float(p[1]))
        elif p[0] == "mola" and len(p) == 2:                 # mola hatırlatıcı aralığını geçici olarak p[1] saniye yap (sınama)
            sess.panel.send("@?", "mola", p[1])
        elif p[0] == "saat" and len(p) == 3:                 # saat başı hatırlatmayı HH:<dakika>'da ve en az <sn> oturumla sına
            sess.panel.send("@?", "saat", p[1], p[2])
        elif p[0] == "drag" and len(p) in (6, 7):            # sanal sürükleme: drag x1 y1 x2 y2 süre_ms [sonda_bekleme_ms]
            sess.panel.send("@?", "drag", *p[1:])
            await asyncio.sleep(int(p[5]) / 1000 + 0.5)       # yalnızca hareket süresi beklenir; sondaki basılı kalma arka planda sürer
        elif p[0] == "perf" and len(p) == 1:                 # çizim ölçümü: öncekinden bu yana kare/sn (panel PERF satırı yazar)
            sess.panel.send("@?", "perf")
            await asyncio.sleep(0.6)
        elif p[0] == "shot" and len(p) == 2:
            sess.shot_done.clear()
            sess.shot_path = str(outdir / f"{p[1]}.png")
            sess.panel.send("@?", "shot")
            try:
                await asyncio.wait_for(sess.shot_done.wait(), 90)
                log(f"Ekran görüntüsü: {sess.shot_path}")
            except asyncio.TimeoutError:
                log(f"Ekran görüntüsü zaman aşımı: {p[1]}")
        else:
            log(f"Geçersiz betik adımı: '{step.strip()}' (go <{'|'.join(SCREEN_IDS)}> | tap x y | wait sn | shot ad)")
    if ARGS.exit_after_script:
        stop.set()


UPDATE = {"want": False, "tries": 0}     # panel yazılım güncellemesi: istek bayrağı ve bu süreçte yapılan deneme sayısı (en çok 2)


async def firmware_watch(sess, stop, cfg):
    """Panel bağlanınca yazılım sürümünü denetler. Paketteki (firmware\\) yazılım panelinkinden yeniyse ya da panel sürüm bildirmeyen eski
    bir yazılımdaysa güncelleme ister ve oturumu bitirir: yüklemeyi ana döngü yapar (esptool seri portu kullanır, port kapalı olmalı).
    Yalnızca kendini Masa Paneli olarak tanıtan (@! / @P yanıtı veren) panellere dokunulur; panel daha yeniyse bir şey yapılmaz."""
    if not cfg["otomatik_yazilim_guncelle"] or ARGS.script or UPDATE["tries"] >= 2:
        return
    loop = asyncio.get_running_loop()
    m, err = await loop.run_in_executor(None, panel_yukle.load_manifest)
    if not m:
        log(f"Panel yazılımı denetlenmedi: {err}")
        return
    for _ in range(6):                          # sürüm bilgisi gelene kadar (en çok ~12 sn) iste
        if sess.fw_version is not None:
            break
        sess.panel.send("@?")
        await asyncio.sleep(2.0)
    ver = sess.fw_version
    if ver is None:
        log("Panel yazılım sürümünü bildirmedi; yazılım denetlenemedi")
        return
    log(f"Panel yazılım sürümü: {ver or 'bildirmiyor (eski)'}; paketteki: {m['surum']}")
    if panel_yukle.needs_update(ver, m["surum"]):
        UPDATE["want"] = True
        stop.set()


def _toast(title, body):
    try:
        notify.show(title, body, persistent=False)
    except Exception as e:  # noqa: BLE001
        log(f"Bildirim gösterilemedi: {e}")


async def update_firmware(port):
    """Paketteki panel yazılımını yükler (ana döngüden, seri port kapalıyken çağrılır). Bilgisayarda bildirimle haber verir."""
    UPDATE["tries"] += 1
    loop = asyncio.get_running_loop()
    m, err = await loop.run_in_executor(None, panel_yukle.load_manifest)
    if not m:
        log(f"Panel yazılımı güncellenemedi: {err}")
        return
    log(f"Panel yazılımı güncelleniyor: sürüm {m['surum']} ({port}, deneme {UPDATE['tries']}/2)")
    await loop.run_in_executor(None, _toast, "Panel yazılımı güncelleniyor", "Yaklaşık 1 dakika sürer; USB kablosunu çıkarma.")
    ok = await loop.run_in_executor(None, panel_yukle.flash, port, m, log)
    if ok:
        ver = await loop.run_in_executor(None, panel_yukle.wait_for_version, port, m["surum"])
        ok = ver == m["surum"]
        if not ok:
            log(f"Yükleme sonrası panel beklenen sürümü bildirmedi ({ver!r})")
    log("Panel yazılımı güncellendi" if ok else "Panel yazılımı güncellenemedi")
    await loop.run_in_executor(None, _toast, "Panel yazılımı güncellendi" if ok else "Panel yazılımı güncellenemedi",
                               f"Sürüm {m['surum']}" if ok else "Ayrıntı: pc-helper\\panel_helper_gunluk.txt")


async def run_once(panel, media, cfg):
    panel.loop = asyncio.get_running_loop()
    panel.rx_lines = asyncio.Queue()
    stop = asyncio.Event()
    sess = Session(panel, media)
    # Oturumu yalnızca gönderici, alıcı ya da durdurma isteği bitirir; yardımcı görevler (sistem izleme, hava durumu,
    # betik) kendiliğinden bitebilir (örn. il tanımlı değil) ve oturumu düşürmemeli.
    core = [asyncio.create_task(sender(sess, stop))]
    if not panel.dry_run:
        core.append(asyncio.create_task(receiver(sess, stop)))
    aux = [asyncio.create_task(sysmon_loop(sess, stop, cfg)), asyncio.create_task(weather_loop(sess, stop, cfg)),
           asyncio.create_task(cover_watch(sess, stop)), asyncio.create_task(session_loop(sess, stop)),
           asyncio.create_task(ext_bridge_loop(sess, stop))]
    if not panel.dry_run:
        aux.append(asyncio.create_task(firmware_watch(sess, stop, cfg)))
    if ARGS.script and not panel.dry_run:
        aux.append(asyncio.create_task(script_runner(sess, stop)))
    stopper = asyncio.create_task(stop.wait())
    done, pending = await asyncio.wait(core + [stopper], return_when=asyncio.FIRST_COMPLETED,
                                       timeout=ARGS.seconds if ARGS.dry_run else None)
    stop.set()
    for t in list(pending) + core + aux:
        if not t.done():
            t.cancel()
    if ARGS.exit_after_script and ARGS.script:
        raise SystemExit(0)


async def main():
    cfg = load_config()
    media = Media(ARGS.source if ARGS.source is not None else cfg["medya_kaynagi"])
    while True:                         # bilgisayar yeni açılırken Windows medya hizmeti henüz hazır olmayabilir
        try:
            await media.start()
            break
        except Exception as e:  # noqa: BLE001
            log(f"Windows medya hizmeti açılamadı ({e}); 5 sn sonra yeniden denenecek")
            await asyncio.sleep(5.0)
    panel = Panel(ARGS.port, ARGS.baud, ARGS.dry_run)
    if ARGS.dry_run:
        log("Deneme kipi: seri port açılmıyor, komut dinlenmiyor")
        await run_once(panel, media, cfg)
        return
    warned = False
    while True:
        try:
            if panel.open():
                warned = False
                await run_once(panel, media, cfg)
                log("Bağlantı sonlandı, yeniden aranıyor")
            elif not warned:
                log("Panel bulunamadı (USB kablosunu ve sürücüyü denetle); aranıyor…")
                warned = True
        except serial.SerialException as e:
            if not warned:
                log(f"Seri port açılamadı: {e}")
                warned = True
        finally:
            panel.close()
        if UPDATE["want"]:                      # port artık kapalı: panel yazılımını güncelle (firmware_watch istedi)
            UPDATE["want"] = False
            try:
                await update_firmware(panel.used_port)
            except Exception as e:  # noqa: BLE001
                log(f"Panel yazılımı güncellenirken hata: {e}")
        await asyncio.sleep(2.0)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Masa paneli yardımcı programı")
    ap.add_argument("--port", help="seri port (varsayılan: CH340'ı otomatik bul)")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--source", default=None,
                    help="medya oturumu kaynağında aranacak metin (varsayılan: panel_ayarlari.json'daki medya_kaynagi, "
                         "o da YouTube Music uygulaması)")
    ap.add_argument("--gunluk", action="store_true", help="çıktıyı konsol yerine panel_helper_gunluk.txt dosyasına yaz "
                    "(konsolsuz çalışırken zaten otomatik)")
    ap.add_argument("--il", help="hava durumu ili (panel_ayarlari.json'daki değerin yerine)")
    ap.add_argument("--ilce", help="hava durumu ilçesi (isteğe bağlı)")
    ap.add_argument("--gpu", choices=["auto", "nvml", "pdh", "none"], default="auto",
                    help="ekran kartı okuma yöntemi (varsayılan: NVIDIA ise NVML, değilse Windows performans sayaçları)")
    ap.add_argument("--dry-run", action="store_true", help="port açma; gönderilecek satırları yazdır")
    ap.add_argument("--seconds", type=float, default=6.0, help="deneme kipinde süre")
    ap.add_argument("--diag", action="store_true", help="panelden 5 sn'de bir durum raporu iste (tanılama)")
    ap.add_argument("--verbose", action="store_true", help="panelin günlük satırlarını da göster")
    ap.add_argument("--script", help="tanılama betiği: 'go <ekran>;tap x y;wait sn;shot ad' adımları (ekranlar: "
                    + ",".join(SCREEN_IDS) + ")")
    ap.add_argument("--script-dir", default="shots", help="ekran görüntülerinin klasörü")
    ap.add_argument("--script-delay", type=float, default=6.0, help="betiğe başlamadan önce beklenecek süre (sn)")
    ap.add_argument("--exit-after-script", action="store_true", help="betik bitince çık")
    ARGS = ap.parse_args()
    setup_logging()
    if not ARGS.dry_run and not single_instance():
        log("Yardımcı program zaten çalışıyor (iki kopya seri portu kapışır); bu kopya kapanıyor. "
            "Tanılama betiği için önce arka plandakini durdur: kur.bat -Durdur")
        sys.exit(2)
    while True:
        try:
            asyncio.run(main())
            break                       # yalnızca deneme kipi kendiliğinden biter
        except KeyboardInterrupt:
            log("Çıkılıyor")
            sys.exit(0)
        except Exception:  # noqa: BLE001  beklenmedik hata: günlüğe yaz, 5 sn sonra baştan başla (arka planda sessizce ölmesin)
            log("Beklenmedik hata, yardımcı yeniden başlatılıyor:\n" + traceback.format_exc())
            time.sleep(5.0)
