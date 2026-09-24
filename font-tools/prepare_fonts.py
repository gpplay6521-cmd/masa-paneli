"""Panel yazı tipi hazırlığı: Google Fonts'tan indirilen değişken (variable) yazı tiplerinden
belirli kalınlıkta (wght) sabit örnek çıkarır ve yalnızca gereken karakterlere indirger.

Çıktı: build/<ad>-<kalınlık>-<latin|jp>.ttf  → sonra lv_font_conv ile LVGL yazı tipine çevrilir.
Kullanım:  .venv\\Scripts\\python -X utf8 prepare_fonts.py
"""
import io
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = Path(__file__).parent
SRC = ROOT / "src"
BUILD = ROOT / "build"
BUILD.mkdir(exist_ok=True)


def latin_codepoints():
    """ASCII + Latin-1 + Latin Extended-A (Türkçe ğ Ğ ı İ ş Ş dahil) + sık noktalama
    + Yunanca + Kiril (Rusça, Ukraynaca, Bulgarca, Sırpça, Kazakça vb. — Manrope'de zaten var)."""
    cps = set(range(0x20, 0x7F))
    cps |= set(range(0xA0, 0x100))
    cps |= set(range(0x100, 0x180))
    cps |= {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2212, 0x00D7}
    cps |= set(range(0x370, 0x400))    # Yunanca
    cps |= set(range(0x400, 0x500))    # Kiril
    return cps


def jis0208_level1_kanji():
    """JIS X 0208 Düzey 1 kanji (2965 karakter): EUC-JP satır 16–47 (baytlar 0xB0–0xCF)."""
    out = set()
    for row in range(0xB0, 0xD0):
        for cell in range(0xA1, 0xFF):
            try:
                ch = bytes([row, cell]).decode("euc_jp")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and 0x4E00 <= ord(ch) <= 0x9FFF:
                out.add(ord(ch))
    return out


def arabic_codepoints():
    """Arapça temel blok (Farsça'ya özgü harfler dahil, örn. پ چ ژ گ) + Arapça Ek + Sunum Formları A/B
    (LVGL'in LV_USE_ARABIC_PERSIAN_CHARS bitişik harf şekillendirmesi bu sunum formu kod noktalarını üretir)."""
    cps = set(range(0x600, 0x700))
    cps |= set(range(0x750, 0x780))
    cps |= set(range(0xFB50, 0xFE00))
    cps |= set(range(0xFE70, 0xFF00))
    return cps


def jp_codepoints():
    cps = set(range(0x3000, 0x3040))   # CJK noktalama
    cps |= set(range(0x3040, 0x30A0))  # hiragana
    cps |= set(range(0x30A0, 0x3100))  # katakana
    cps |= set(range(0xFF01, 0xFF5F))  # tam genişlikli ASCII
    cps |= set(range(0xFF61, 0xFFA0))  # yarım genişlikli katakana
    cps |= jis0208_level1_kanji()
    return cps


def make(src_name, out_stem, wght, cps, extra_axes=None):
    font = TTFont(SRC / src_name)
    cmap = font.getBestCmap()
    have = {c for c in cps if c in cmap}
    missing = sorted(cps - have)
    # Önce indirge (hızlı), sonra kalınlığı sabitle.
    opts = subset.Options()
    opts.layout_features = ["kern", "liga", "locl"]
    opts.notdef_outline = True
    opts.name_IDs = ["*"]
    opts.drop_tables += ["DSIG"]
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=sorted(have))
    sub.subset(font)
    axes = {"wght": wght, **(extra_axes or {})}
    static = instancer.instantiateVariableFont(font, axes, inplace=False)
    out = BUILD / f"{out_stem}.ttf"
    static.save(out)
    print(f"{out.name}: {len(have)} karakter, dosya {out.stat().st_size/1024:.0f} KB, yazı tipinde olmayan: {len(missing)}")
    if missing:
        print("   eksik örnekler:", " ".join(chr(c) for c in missing[:20]))
    return have


if __name__ == "__main__":
    latin = latin_codepoints()
    jp = jp_codepoints()
    ar = arabic_codepoints()
    print(f"istenen: latin={len(latin)}  jp={len(jp)} (kanji L1 = {len(jis0208_level1_kanji())})  arapca/farsca={len(ar)}")
    for w in (600, 700, 800):
        make("Manrope[wght].ttf", f"Manrope-{w}-latin", w, latin)
    for w in (600, 800):
        make("NotoSansJP[wght].ttf", f"NotoSansJP-{w}-jp", w, jp)
    for w in (600, 800):
        make("NotoSansArabic[wdth,wght].ttf", f"NotoSansArabic-{w}-ar", w, ar, extra_axes={"wdth": 100})
