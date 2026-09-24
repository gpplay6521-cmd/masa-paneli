"""Kapak resmi: Windows medya oturumundaki küçük resmi (SMTC thumbnail) panele uygun 128x128 RGB565'e çevirir.

Kalite için: merkezden kare kırpma, Lanczos ile küçültme, hafif keskinleştirme ve Floyd-Steinberg dithering
(panel 16 bit renk gösterir; dithering 5-6-5 kuantizasyonundaki bantlaşmayı gizler).
Aktarım: @G/@I/@E satırları (proto.h); veri base64, panel CRC32 ile doğrular.
"""
import base64
import io
import zlib

from PIL import Image, ImageFilter, ImageOps
from winrt.windows.storage.streams import Buffer, InputStreamOptions

COVER = 128
CHUNK = 576          # ham bayt/satır -> 768 base64 karakter (panelin satır tamponu 1100)
MAX_BYTES = 8_000_000


async def read_thumbnail(props):
    """Medya özelliklerindeki küçük resmin ham (JPEG/PNG) baytları; yoksa None."""
    ref = props.thumbnail
    if ref is None:
        return None
    stream = await ref.open_read_async()
    size = int(stream.size)
    if size <= 0 or size > MAX_BYTES:
        return None
    buf = Buffer(size)
    await stream.read_async(buf, size, InputStreamOptions.READ_AHEAD)
    return bytes(memoryview(buf))[: int(buf.length)]


def dither565(img):
    """RGB PIL görüntüsü -> RGB565 (küçük uçlu) baytları, Floyd-Steinberg dithering ile."""
    w, h = img.size
    src = img.tobytes()
    out = bytearray(w * h * 2)
    scale = (255 / 31, 255 / 63, 255 / 31)
    cur = [[0.0] * (w + 2) for _ in range(3)]
    nxt = [[0.0] * (w + 2) for _ in range(3)]
    o = 0
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 3
            q = [0, 0, 0]
            for c in range(3):
                v = src[i + c] + cur[c][x + 1]
                v = 0.0 if v < 0 else (255.0 if v > 255 else v)
                lv = int(v / scale[c] + 0.5)
                q[c] = lv
                e = v - lv * scale[c]
                cur[c][x + 2] += e * 7 / 16
                nxt[c][x] += e * 3 / 16
                nxt[c][x + 1] += e * 5 / 16
                nxt[c][x + 2] += e * 1 / 16
            val = (q[0] << 11) | (q[1] << 5) | q[2]
            out[o] = val & 255
            out[o + 1] = val >> 8
            o += 2
        cur, nxt = nxt, [[0.0] * (w + 2) for _ in range(3)]
    return bytes(out)


def render_565(data, size=COVER):
    """Ham resim baytları -> size x size RGB565 baytları (size*size*2)."""
    img = Image.open(io.BytesIO(data))
    img.load()
    img = ImageOps.exif_transpose(img).convert("RGB")
    w, h = img.size
    s = min(w, h)
    left, top = (w - s) // 2, (h - s) // 2
    img = img.crop((left, top, left + s, top + s)).resize((size, size), Image.LANCZOS)
    img = img.filter(ImageFilter.UnsharpMask(radius=0.7, percent=45, threshold=2))
    return dither565(img)


def save_png(data, path):
    """Ham küçük resmi PNG olarak kaydet; (genişlik, yükseklik) döndür. Tanılama için: panelde yanlış bir resim görülürse
    en son gönderilen birkaç kapak kapak_son klasöründe durur."""
    img = Image.open(io.BytesIO(data))
    img.load()
    img.convert("RGB").save(path)
    return img.size


def frames(data565, cid):
    """Aktarılacak protokol satırları (alan listeleri)."""
    crc = zlib.crc32(data565) & 0xFFFFFFFF
    yield ("@G", cid, COVER, COVER, f"{crc:08x}", len(data565))
    for off in range(0, len(data565), CHUNK):
        yield ("@I", cid, off, base64.b64encode(data565[off:off + CHUNK]).decode("ascii"))
    yield ("@E", cid)


def clear_frame(cid):
    return ("@G", cid, 0, 0, "0", 0)


def rgb565_to_image(data565, w, h):
    """Panelden gelen RGB565 (küçük uçlu) baytlarını PIL görüntüsüne çevir (ekran görüntüsü / sınama için)."""
    px = bytearray(w * h * 3)
    for i in range(w * h):
        v = data565[2 * i] | (data565[2 * i + 1] << 8)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px[3 * i] = (r << 3) | (r >> 2)
        px[3 * i + 1] = (g << 2) | (g >> 4)
        px[3 * i + 2] = (b << 3) | (b >> 2)
    return Image.frombytes("RGB", (w, h), bytes(px))
