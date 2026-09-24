"""Alt küme kapsam denetimi + görsel önizleme.

1) Tasarımdaki tüm metinler ve örnek Türkçe/Japonca satırlar, üretilen alt kümelerde var mı?
2) LVGL'deki 'fallback' mantığını taklit ederek (önce Latin, yoksa Japonca yazı tipi) bir PNG çizer.

Kullanım:  .venv\\Scripts\\python -X utf8 check_coverage.py
"""
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).parent
B = ROOT / "build"

SAMPLES = [
    # (metin, boyut, kalınlık, açıklama)
    ("Şarkı Adı", 34, 800, "başlık"),
    ("Uzun Bir Şarkı Adı Nasıl Görünür (Canlı Versiyon)", 34, 800, "uzun başlık"),
    ("日本語の曲名", 34, 800, "Japonca başlık"),
    ("夜空と星の歌 – 青春の記憶 (Live)", 34, 800, "karışık Japonca"),
    ("Sanatçı Adı, Konuk Sanatçı", 22, 600, "sanatçı"),
    ("アーティスト名 · かみ", 22, 600, "Japonca sanatçı"),
    ("Şimdi çalıyor · Duraklatıldı · Boşta karart · Dokununca uyanır", 22, 600, "etiketler"),
    ("ĞÜŞİÖÇ ğüşıöç — Bilgisayar bekleniyor · Yeniden dene · Gökyüzü", 22, 600, "Türkçe harfler"),
    ("0123456789 :% 30 sn · 2 dk · 10 dk · Kapalı · 1:24 / 3:47", 22, 600, "rakamlar"),
]


def cmap_of(path):
    return set(TTFont(path).getBestCmap().keys())


def main():
    latin = {w: cmap_of(B / f"Manrope-{w}-latin.ttf") for w in (600, 800)}
    jp = {w: cmap_of(B / f"NotoSansJP-{w}-jp.ttf") for w in (600, 800)}
    ok_all = True
    for text, size, w, label in SAMPLES:
        miss = [c for c in text if ord(c) not in latin[w] and ord(c) not in jp[w]]
        status = "TAMAM" if not miss else "EKSİK: " + "".join(sorted(set(miss)))
        ok_all &= not miss
        print(f"{label:<18} {status}")
    print("\nGenel sonuç:", "hepsi kapsanıyor" if ok_all else "eksik karakter var")

    # Görsel önizleme (Pillow, LVGL fallback mantığı: Latin yoksa Japonca)
    pad = 24
    row_h = 64
    img = Image.new("RGB", (1040, pad * 2 + row_h * len(SAMPLES)), (27, 20, 64))
    d = ImageDraw.Draw(img)
    for i, (text, size, w, label) in enumerate(SAMPLES):
        fl = ImageFont.truetype(str(B / f"Manrope-{w}-latin.ttf"), size)
        fj = ImageFont.truetype(str(B / f"NotoSansJP-{w}-jp.ttf"), size)
        x, y = pad, pad + i * row_h
        for ch in text:
            f = fl if ord(ch) in latin[w] else fj
            d.text((x, y), ch, font=f, fill=(244, 245, 247))
            x += d.textlength(ch, font=f)
    out = ROOT / "build" / "onizleme.png"
    img.save(out)
    print("önizleme:", out)


if __name__ == "__main__":
    main()
