"""Panelin seri portu: bulma ve yoklama (panel_helper.py ile panel_yukle.py ortak kullanır).

Panel USB'de CH340K (VID 0x1A86) olarak görünür. Bir bilgisayarda başka CH340 aygıtı da olabilir (Arduino klonu vb.); bu yüzden bir port
"panel mi" diye '@?' gönderip panelin '@!' yanıtına bakılarak anlaşılır. Yanıtın son alanı panelin yazılım sürümüdür (eski yazılımda yok).
"""
import time

import serial
import serial.tools.list_ports

CH340_VID = 0x1A86
BAUD = 921600                    # panel yazılımındaki PROTO_BAUD ile aynı


def candidates():
    """CH340 (panelin USB-seri çipi) aygıtlarının port adları."""
    return [p.device for p in serial.tools.list_ports.comports() if p.vid == CH340_VID]


def query(device, timeout=3.0):
    """Portta Masa Paneli yazılımı var mı? '@?' gönderir, '@!' yanıtını bekler.
    Döndürür: None = yanıt yok (panel değil / boş ya da başka yazılım / port meşgul);
              ''   = panel var ama sürüm bildirmiyor (eski yazılım);
              'YYYY.AA.GG.n' = panelin yazılım sürümü."""
    try:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = device, BAUD, 0.2
        s.dtr = False            # kartı sıfırlayan DTR/RTS hatlarını boşta tut
        s.rts = False
        s.open()
    except Exception:  # noqa: BLE001
        return None
    try:
        buf = b""
        end = time.time() + timeout
        last_ask = 0.0
        while time.time() < end:
            if time.time() - last_ask > 0.8:
                s.write(b"@?\n")
                last_ask = time.time()
            buf += s.read(512)
            for line in buf.split(b"\n"):
                line = line.decode("utf-8", "replace").rstrip("\r")
                if line.startswith("@!\t"):
                    f = line.split("\t")
                    return f[7].strip() if len(f) >= 8 else ""
    except Exception:  # noqa: BLE001
        return None
    finally:
        s.close()
    return None


def find_panel(prefer=None):
    """(port, sürüm) — panel yazılımının yanıt verdiği ilk CH340 portu; yoksa (None, None).
    prefer: önce denenecek port (örn. kullanıcı verdi)."""
    cands = candidates()
    if prefer and prefer not in cands:
        cands.insert(0, prefer)
    elif prefer:
        cands.remove(prefer)
        cands.insert(0, prefer)
    for dev in cands:
        ver = query(dev)
        if ver is not None:
            return dev, ver
    return None, None
