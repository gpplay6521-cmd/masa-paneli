"""Yükleyici paketini hazırlar: derlenmiş panel yazılımını (panel-firmware\\build) pc-helper\\firmware\\ altına kopyalar ve manifest.json yazar.

Manifest: sürüm (panel-firmware\\main\\fw_version.h), çip/flash ayarları ve dosya ofsetleri (ESP-IDF'in ürettiği flasher_args.json'dan),
her dosyanın SHA-256 özeti. Yükleyici (panel_yukle.py) yüklemeden önce özetleri denetler.
Eski bir derlemeyi yanlışlıkla paketlememek için: fw_version.h'daki sürüm dizgesi derlenmiş uygulamanın içinde geçmiyorsa durur
(önce `idf.py build`).

Kullanım:  .venv\\Scripts\\python -X utf8 firmware_hazirla.py        (paketle.ps1 bunu kendisi çağırır)
"""
import hashlib
import json
import re
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).parent
BUILD = HERE.parent / "panel-firmware" / "build"
VERSION_H = HERE.parent / "panel-firmware" / "main" / "fw_version.h"
OUT = HERE / "firmware"


def sha256(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    m = re.search(r'#define\s+FW_VERSION\s+"([0-9.]+)"', VERSION_H.read_text(encoding="utf-8"))
    if not m:
        sys.exit(f"HATA: {VERSION_H} içinde FW_VERSION bulunamadı")
    version = m.group(1)
    fa = json.loads((BUILD / "flasher_args.json").read_text(encoding="utf-8"))
    files = fa["flash_files"]                          # {"0x0": "bootloader/bootloader.bin", ...}
    app_rel = fa["app"]["file"]
    if version.encode() not in (BUILD / app_rel).read_bytes():
        sys.exit(f"HATA: derlenmiş {app_rel} içinde sürüm {version} geçmiyor; önce derle: idf.py build")
    OUT.mkdir(exist_ok=True)
    entries = []
    for offset, rel in sorted(files.items(), key=lambda kv: int(kv[0], 16)):
        name = Path(rel).name
        shutil.copyfile(BUILD / rel, OUT / name)
        entries.append({"ofset": offset, "dosya": name, "sha256": sha256(OUT / name), "bayt": (OUT / name).stat().st_size})
    for old in OUT.glob("*.bin"):                       # artık listede olmayan eski dosyaları temizle
        if old.name not in {e["dosya"] for e in entries}:
            old.unlink()
    manifest = {
        "surum": version,
        "chip": fa["extra_esptool_args"]["chip"],
        "flash": fa["flash_settings"],                  # mode / size / freq
        "dosyalar": entries,
        "uretim": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    total = sum(e["bayt"] for e in entries)
    print(f"Panel yazılımı paketi hazır: sürüm {version}, {len(entries)} dosya, {total / 1024:.0f} KB -> {OUT}")


main()
