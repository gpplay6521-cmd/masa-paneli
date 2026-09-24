"""Panel yazılımı yükleyicisi (uploader): pc-helper\\firmware\\ içindeki hazır yazılımı panele yükler. ESP-IDF/derleyici GEREKMEZ (esptool yeter).

Kullanım (kur.bat bunu kendisi çağırır):
  panel_yukle.py                paneli bulur; yazılımı eskiyse ya da sürüm bildirmiyorsa yükler, güncelse hiçbir şey yapmaz
  panel_yukle.py --kontrol      yalnızca panelin ve paketteki sürümü yazar
  panel_yukle.py --zorla        sürüm ne olursa olsun yükler (yeniden yükleme)
  panel_yukle.py --otomatik     soru sormaz: yalnızca kendini tanıtan (Masa Paneli yazılımı çalışan) ve sürümü eski olan paneli günceller
  panel_yukle.py --port COM4    portu elle ver
  panel_yukle.py --evet         panel yanıt vermiyorsa (boş/fabrika yazılımı) ve ESP32-S3 16 MB olduğu doğrulanırsa sormadan yükle
  panel_yukle.py --yardimciya-dokunma   çalışan yardımcı programı durdurup yeniden başlatma (kur.bat / yardımcı kendisi çağırırken)

Güvenlik: yükleme öncesi dosyaların SHA-256 özetleri manifestle karşılaştırılır; kendini tanıtmayan bir cihaza yalnızca çip "ESP32-S3" ve
flash "16MB" olarak doğrulanırsa (ve kullanıcı onaylarsa) yazılır; Arduino klonu gibi başka CH340 aygıtlarına dokunulmaz.
Yükleme sonrası panelin yeni sürümle yeniden başladığı doğrulanır. Yardımcı program da panel bağlanınca aynı sürüm denetimini kendiliğinden yapar
(panel_ayarlari.json "otomatik_yazilim_guncelle").
Çıkış kodu: 0 tamam/güncel, 1 hata, 2 panel USB'de yok, 3 cihaz yanıt vermedi (otomatik kipte dokunulmadı).
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import psutil

import panelport

HERE = Path(__file__).parent
FW_DIR = HERE / "firmware"
FLASH_BAUD = 460800                 # idf.py flash'ın varsayılanı; bu panelde doğrulandı
CREATE_NO_WINDOW = 0x08000000
DETACHED_PROCESS = 0x00000008


def _log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def _sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_manifest():
    """(manifest, hata). Dosyalar var ve SHA-256 özetleri manifestle tutuyorsa manifest, yoksa None ve nedeni."""
    try:
        m = json.loads((FW_DIR / "manifest.json").read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None, "firmware\\manifest.json yok (firmware_hazirla.py ile hazırlanır)"
    except Exception as e:  # noqa: BLE001
        return None, f"manifest okunamadı: {e}"
    if not m.get("surum") or not m.get("dosyalar"):
        return None, "manifest eksik"
    for d in m["dosyalar"]:
        p = FW_DIR / d["dosya"]
        if not p.is_file():
            return None, f"{d['dosya']} yok"
        if _sha256(p) != d["sha256"]:
            return None, f"{d['dosya']} özeti manifestle tutmuyor (bozuk ya da eksik kopya)"
    return m, None


def parse_version(s):
    try:
        return tuple(int(x) for x in (s or "").strip().split("."))
    except ValueError:
        return ()


def needs_update(panel_ver, bundle_ver):
    """Panel eskiyse (ya da sürüm bildirmeyen eski yazılımdaysa) True. Eşit ya da panel daha yeniyse False (geri sürüm yüklenmez)."""
    pv = parse_version(panel_ver)
    return not pv or pv < parse_version(bundle_ver)


def python_exe():
    """esptool'u konsol penceresi açmadan çalıştırmak için python.exe (pythonw değil)."""
    exe = Path(sys.executable)
    if exe.name.lower() == "pythonw.exe" and exe.with_name("python.exe").exists():
        return str(exe.with_name("python.exe"))
    return str(exe)


def _esptool(args, log, progress=False):
    """esptool'u alt süreç olarak çalıştırır; (çıkış kodu, satırlar)."""
    cmd = [python_exe(), "-X", "utf8", "-m", "esptool", *args]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                         creationflags=CREATE_NO_WINDOW, cwd=str(HERE))
    lines, mark, last = [], 25, 0.0
    for line in p.stdout:
        line = line.rstrip()
        if not line:
            continue
        lines.append(line)
        if progress:
            m = re.search(r"(\d+(?:\.\d+)?) ?%", line)
            if m and "Writing at" in line:
                pct = float(m.group(1))
                if pct < last - 50:                 # yeni dosyaya geçildi (yüzde başa döndü)
                    mark = 25
                last = pct
                if pct >= mark and pct - 25 < mark:  # her dosyada %25/%50/%75/%100'de bir satır; küçük dosyalar sessiz geçer
                    log(f"  yükleniyor: %{int(pct)}")
                    mark += 25
    return p.wait(), lines


def identify(port, log=_log):
    """Kendini tanıtmayan bir cihazda: (çip, flash boyutu) — örn. ('ESP32-S3 (QFN56) (revision v0.2)', '16MB'); okunamazsa (None, None)."""
    rc, out = _esptool(["--port", port, "flash-id"], log)
    text = "\n".join(out)
    chip = re.search(r"Chip type:\s+(.+)", text)
    size = re.search(r"Detected flash size:\s+(\S+)", text)
    return (chip.group(1).strip() if chip else None, size.group(1) if size else None)


def flash(port, m, log=_log, baud=FLASH_BAUD):
    """Manifestteki dosyaları panele yazar (esptool: sıkıştırılmış yazma + doğrulama). Başarılıysa True. Çalışan yardımcı programın portu KAPALI olmalı."""
    fl = m["flash"]
    args = ["--chip", m["chip"], "--port", port, "--baud", str(baud), "--before", "default-reset", "--after", "hard-reset",
            "write-flash", "--flash-mode", fl["flash_mode"], "--flash-size", fl["flash_size"], "--flash-freq", fl["flash_freq"]]
    for d in m["dosyalar"]:
        args += [d["ofset"], str(FW_DIR / d["dosya"])]
    log(f"Panel yazılımı yükleniyor: sürüm {m['surum']} -> {port} (yaklaşık 1 dakika; USB kablosunu çıkarma)")
    rc, out = _esptool(args, log, progress=True)
    verified = sum("Hash of data verified" in l for l in out)
    if rc != 0 or verified != len(m["dosyalar"]):
        log(f"Yükleme BAŞARISIZ (çıkış kodu {rc}, doğrulanan dosya {verified}/{len(m['dosyalar'])}). esptool çıktısının sonu:")
        for l in out[-8:]:
            log("  " + l)
        return False
    return True


def wait_for_version(port, expect, timeout=45.0):
    """Yükleme sonrası panel yeniden başlayıp sürümünü bildirene kadar bekler; bildirilen sürümü (ya da None) döndürür."""
    end = time.time() + timeout
    time.sleep(3.0)
    while time.time() < end:
        ver = panelport.query(port, timeout=2.0)
        if ver is not None:
            return ver
        time.sleep(1.0)
    return None


# --- çalışan yardımcı programı durdur / yeniden başlat (yalnızca komut satırından çağrılınca) ---
def _helper_procs():
    out = []
    for p in psutil.process_iter(["pid", "cmdline"]):
        try:
            if "panel_helper.py" in " ".join(p.info["cmdline"] or []) and p.pid != os.getpid():
                out.append(p)
        except psutil.Error:
            pass
    return out


def stop_helper():
    procs = _helper_procs()
    for p in procs:
        try:
            p.kill()
        except psutil.Error:
            pass
    psutil.wait_procs(procs, timeout=5)
    return bool(procs)


def start_helper():
    pyw = Path(sys.executable).with_name("pythonw.exe")
    if not pyw.exists():
        pyw = Path(sys.executable)
    subprocess.Popen([str(pyw), "-X", "utf8", str(HERE / "panel_helper.py")], cwd=str(HERE),
                     creationflags=DETACHED_PROCESS | CREATE_NO_WINDOW, close_fds=True)


def _do_flash(port, m, args):
    if not flash(port, m):
        return 1
    ver = wait_for_version(port, m["surum"])
    if ver == m["surum"]:
        _log(f"Panel yeniden başladı; yazılım sürümü {ver}. Yükleme tamam.")
        return 0
    _log(f"UYARI: yazılım yazıldı ve doğrulandı ama panel beklenen sürümü ({m['surum']}) bildirmedi (bildirilen: {ver!r}). "
         "Kabloyu çıkarıp takmayı dene.")
    return 1


def main():
    ap = argparse.ArgumentParser(description="Masa paneli yazılım yükleyicisi")
    ap.add_argument("--port")
    ap.add_argument("--kontrol", action="store_true")
    ap.add_argument("--zorla", action="store_true")
    ap.add_argument("--otomatik", action="store_true")
    ap.add_argument("--evet", action="store_true")
    ap.add_argument("--yardimciya-dokunma", action="store_true")
    args = ap.parse_args()

    m, err = load_manifest()
    if not m:
        if args.otomatik:                    # panelin taşıdığı "hafif" kurulum panel yazılımı içermez (panelin zaten kendi yazılımı var)
            _log(f"Bu kurulum panel yazılımı paketi içermiyor; yazılım denetimi atlandı ({err}).")
            return 0
        _log(f"HATA: yazılım paketi kullanılamıyor: {err}")
        return 1
    _log(f"Pakettaki panel yazılımı: sürüm {m['surum']} ({len(m['dosyalar'])} dosya, özetler doğrulandı)")

    helper_was_running = False
    if not args.yardimciya_dokunma:
        helper_was_running = stop_helper()      # çalışan yardımcı portu tutar; işimiz bitince geri başlatılır
        if helper_was_running:
            _log("Çalışan yardımcı program geçici olarak durduruldu.")
            time.sleep(1.0)
    try:
        port, ver = panelport.find_panel(args.port)
        if port is None:
            cands = panelport.candidates()
            if not cands:
                _log("Panel USB'de görünmüyor. Veri kablosuyla panelin USB-UART (CH340) portuna tak; yalnızca şarj kablosu çalışmaz.")
                return 2
            if args.otomatik or args.kontrol:
                _log(f"CH340 aygıtı var ({', '.join(cands)}) ama Masa Paneli yazılımı yanıt vermedi (port başka bir programda açık olabilir, "
                     "örn. çalışan yardımcı); otomatik kipte dokunulmadı (elle: panel_yukle.bat).")
                return 3
            port = args.port or cands[0]
            _log(f"{port} portundaki cihaz Masa Paneli yazılımıyla yanıt vermedi (boş ya da fabrika yazılımı olabilir); çip denetleniyor...")
            chip, size = identify(port)
            _log(f"  çip: {chip}, flash: {size}")
            if not chip or "ESP32-S3" not in chip or size != "16MB":
                _log("Bu cihaz ESP32-S3 / 16 MB flash olarak doğrulanamadı; güvenlik için hiçbir şey yazılmadı.")
                return 1
            if not args.evet:
                cevap = input(f"{port} üzerindeki ESP32-S3'e Masa Paneli yazılımı {m['surum']} yüklensin mi? Cihazdaki mevcut yazılım silinir. (e/h): ")
                if cevap.strip().lower() not in ("e", "evet", "y", "yes"):
                    _log("Vazgeçildi.")
                    return 1
            return _do_flash(port, m, args)

        _log(f"Panel bulundu: {port}; yazılım sürümü: {ver or 'bildirmiyor (eski yazılım)'}")
        if args.kontrol:
            _log("Güncelleme gerekli." if needs_update(ver, m["surum"]) else "Panel yazılımı güncel.")
            return 0
        if not args.zorla and not needs_update(ver, m["surum"]):
            _log("Panel yazılımı güncel; yapılacak bir şey yok.")
            return 0
        return _do_flash(port, m, args)
    finally:
        if helper_was_running:
            start_helper()
            _log("Yardımcı program yeniden başlatıldı.")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(1)
