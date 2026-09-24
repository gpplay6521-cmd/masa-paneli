"""Panel yazılımı için LVGL yazı tiplerini üretir (lv_font_conv, --format lvgl).

Girdi : build\\*.ttf (prepare_fonts.py çıktısı) ve src\\FontAwesome5-Solid+Brands+Regular.woff (LVGL'in resmi simge yazı tipi)
Çıktı : ..\\panel-firmware\\main\\fonts\\*.c ve fonts.h

Tip ölçeği (tasarım tuvalindeki değerlerin sadeleştirilmiş hali; boyut px / kalınlık):
  saat 148/800 (yalnız rakam) · sayac 88/800 (yalnız rakam, zamanlayıcı) · xl 36/800 · baslik 34/800(+JP) · h1 30/800 · govde_b 22/600(+JP)
  govde 20/600(+JP) · etiket 18/700 · kucuk 16/600
Kullanım:  .venv\\Scripts\\python -X utf8 build_panel_fonts.py
"""
import subprocess
import sys
from pathlib import Path

from fontTools.ttLib import TTFont

ROOT = Path(__file__).parent
BUILD = ROOT / "build"
OUT = ROOT.parent / "panel-firmware" / "main" / "fonts"
FA = ROOT / "src" / "FontAwesome5-Solid+Brands+Regular.woff"
LVCONV = ROOT / "node_modules" / ".bin" / "lv_font_conv.cmd"

# Kullanılan Font Awesome 5 Free (Solid) simgeleri: ad -> kod noktası
ICONS = {
    "play": 0xF04B, "pause": 0xF04C, "prev": 0xF048, "next": 0xF051,
    "redo": 0xF01E, "undo": 0xF0E2, "palette": 0xF53F, "sliders": 0xF1DE,
    "sun": 0xF185, "moon": 0xF186, "left": 0xF053, "right": 0xF054,
    "check": 0xF00C, "sync": 0xF021, "plug": 0xF1E6, "desktop": 0xF108,
    # hava durumu (MGM hadise kodları için)
    "cloud": 0xF0C2, "cloud_sun": 0xF6C4, "cloud_moon": 0xF6C3, "cloud_rain": 0xF73D,
    "cloud_heavy": 0xF740, "cloud_sun_rain": 0xF743, "cloud_moon_rain": 0xF73C,
    "cloud_hail": 0xF73B, "snow": 0xF2DC, "bolt": 0xF0E7, "smog": 0xF75F, "wind": 0xF72E,
    "hot": 0xF769, "cold": 0xF76B,
    # sistem izleme, zamanlayıcı, kapak yer tutucusu
    "chip": 0xF2DB, "memory": 0xF538, "cube": 0xF1B2, "layers": 0xF5FD,
    "stopwatch": 0xF2F2, "bell": 0xF0F3, "plus": 0xF067, "minus": 0xF068,
    "coffee": 0xF0F4, "brain": 0xF5DC, "music": 0xF001,
    # sistem ekranı alt şeridi: ağ hızı, gecikme, oturum süresi
    "arrow_down": 0xF063, "arrow_up": 0xF062, "wifi": 0xF1EB, "hourglass": 0xF252,
}
ICON_SIZES = [26, 30, 34, 36, 46, 56]

# (ad, Manrope kalınlığı, boyut, aralık ya da None=tüm alt küme, yedek yazı tipi)
LATIN = [
    ("saat", 800, 148, "0x30-0x3A", None),
    ("sayac", 800, 88, "0x30-0x3A", None),
    ("mini", 800, 12, "0x30-0x3A", None),
    ("xl", 800, 36, None, None),
    ("baslik", 800, 34, None, "jp34"),
    ("h1", 800, 30, None, None),
    ("govde_b", 600, 22, None, "jp22"),
    ("govde", 600, 20, None, "jp20"),
    ("etiket", 700, 18, None, None),
    ("kucuk", 600, 16, None, None),
]
# (ad, Noto Sans JP kalınlığı, boyut)
JP = [("jp34", 800, 34), ("jp22", 600, 22), ("jp20", 600, 20)]
# (ad, Noto Sans Arabic kalınlığı, boyut) — Latin/Kiril/Yunanca'da (baslik/govde_b/govde) bulunmayan
# karakter jp'ye, jp'de de bulunmayan karakter buna düşer (Arapça/Farsça, bitişik harf şekilleriyle)
AR = [("ar34", 800, 34), ("ar22", 600, 22), ("ar20", 600, 20)]
BPP = 4


def conv(font, name, size, ranges, fallback=None):
    out = OUT / f"font_{name}.c"
    cmd = [str(LVCONV), "--font", str(font)]
    for r in ranges:
        cmd += ["-r", r]
    cmd += ["--size", str(size), "--bpp", str(BPP), "--format", "lvgl",
            "--lv-include", "lvgl.h", "--lv-font-name", f"font_{name}", "-o", str(out)]
    if fallback:
        cmd += ["--lv-fallback", f"font_{fallback}"]
    r = subprocess.run(cmd, capture_output=True, text=True, shell=False)
    if r.returncode != 0:
        print("HATA", name, r.stderr[:400])
        sys.exit(1)
    print(f"font_{name}: {size}px {out.stat().st_size / 1024:.0f} KB (C kaynağı)")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    fa_cmap = TTFont(FA).getBestCmap()
    missing = [n for n, c in ICONS.items() if c not in fa_cmap]
    if missing:
        print("Font Awesome'da olmayan simgeler:", missing)
        sys.exit(1)

    names = []
    for name, w, size, rng, fb in LATIN:
        conv(BUILD / f"Manrope-{w}-latin.ttf", name, size, [rng or "0x20-0xFFFF"], fb)
        names.append(name)
    for (name, w, size), (ar_name, _, _) in zip(JP, AR):
        conv(BUILD / f"NotoSansJP-{w}-jp.ttf", name, size, ["0x20-0xFFFF"], ar_name)
        names.append(name)
    for name, w, size in AR:
        conv(BUILD / f"NotoSansArabic-{w}-ar.ttf", name, size, ["0x20-0xFFFF"])
        names.append(name)
    codes = ",".join(f"0x{c:X}" for c in ICONS.values())
    for size in ICON_SIZES:
        conv(FA, f"ikon{size}", size, [codes])
        names.append(f"ikon{size}")

    with open(OUT / "fonts.h", "w", encoding="utf-8") as f:
        f.write("/* Otomatik üretildi: font-tools/build_panel_fonts.py — elle düzenlemeyin. */\n")
        f.write("#pragma once\n#include \"lvgl.h\"\n\n")
        for n in names:
            f.write(f"LV_FONT_DECLARE(font_{n})\n")
        f.write("\n/* Font Awesome simgeleri (UTF-8) */\n")
        for n, c in ICONS.items():
            f.write(f"#define ICON_{n.upper()} \"{chr(c).encode('utf-8').decode('latin-1').encode('unicode_escape').decode()}\"\n")
    print("fonts.h yazıldı")


if __name__ == "__main__":
    main()
