"""Windows medya oturumu (SMTC) sondası: YALNIZCA OKUR, hiçbir şeyi kontrol etmez.

Bilgisayarda çalan medyanın (ör. Chrome'daki YouTube Music) panele hangi bilgileri
verebileceğini gösterir: başlık, sanatçı, kapak, süre/konum, açık olan denetimler.

Kullanım:  .venv\\Scripts\\python probe_smtc.py [--save-cover DOSYA_YOLU]
"""
import argparse
import asyncio
import io
import time

from PIL import Image
from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as SessionManager,
)
from winrt.windows.storage.streams import Buffer, DataReader, InputStreamOptions


async def read_thumbnail(props):
    """Kapak resminin ham baytlarını ve içerik türünü döndürür (yoksa None)."""
    if props.thumbnail is None:
        return None, None
    stream = await props.thumbnail.open_read_async()
    size = int(stream.size)
    buf = Buffer(size)
    await stream.read_async(buf, buf.capacity, InputStreamOptions.READ_AHEAD)
    reader = DataReader.from_buffer(buf)
    data = bytearray(reader.unconsumed_buffer_length)
    reader.read_bytes(data)
    return bytes(data), stream.content_type


def describe_session(session):
    pb = session.get_playback_info()
    c = pb.controls
    print("  durum:", pb.playback_status, "| tür:", pb.playback_type)
    print(
        "  denetimler: play=%s pause=%s next=%s prev=%s seek=%s shuffle=%s repeat=%s"
        % (
            c.is_play_enabled,
            c.is_pause_enabled,
            c.is_next_enabled,
            c.is_previous_enabled,
            c.is_playback_position_enabled,
            c.is_shuffle_enabled,
            c.is_repeat_enabled,
        )
    )
    print("  shuffle aktif:", pb.is_shuffle_active, "| tekrar modu:", pb.auto_repeat_mode)


async def main(save_cover):
    mgr = await SessionManager.request_async()
    sessions = list(mgr.get_sessions())
    print("oturum sayısı:", len(sessions))
    for s in sessions:
        print("--- kaynak uygulama:", s.source_app_user_model_id)
        props = await s.try_get_media_properties_async()
        print("  başlık:", repr(props.title))
        print("  sanatçı:", repr(props.artist), "| albüm:", repr(props.album_title))
        describe_session(s)

        data, ctype = await read_thumbnail(props)
        if data is None:
            print("  kapak: yok")
        else:
            img = Image.open(io.BytesIO(data))
            print("  kapak: tür=%r bayt=%d biçim=%s boyut=%dx%d" % (ctype, len(data), img.format, *img.size))
            if save_cover:
                with open(save_cover, "wb") as f:
                    f.write(data)
                print("  kapak kaydedildi:", save_cover)

        # Zaman çizelgesi 3 sn arayla iki kez: canlı akıyor mu?
        for i in (1, 2):
            tl = s.get_timeline_properties()
            print(
                "  zaman çizelgesi %d: konum=%.1fs süre=%.1fs son_güncelleme=%s"
                % (i, tl.position.total_seconds(), tl.end_time.total_seconds(), tl.last_updated_time.strftime("%H:%M:%S"))
            )
            if i == 1:
                time.sleep(3)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--save-cover", help="kapak resmini bu dosyaya kaydet")
    asyncio.run(main(ap.parse_args().save_cover))
