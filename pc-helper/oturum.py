"""Oturum süresi: bilgisayar başında kesintisiz geçen süre (mola hatırlatıcı ve sistem ekranı için).

Windows'un "son giriş" zamanı (GetLastInputInfo: klavye ve fare) kullanılır; hareketsizlik 5 dakikayı geçerse oturum bitmiş sayılır ve
kişi geri dönünce süre sıfırdan başlar. Yardımcı program başladığında kişi zaten bilgisayardaysa süre yardımcının başladığı andan sayılır
(önceki oturumun ne zaman başladığı bilinmez).
⚠ Yalnızca klavye/fare sayılır; sadece oyun kolu (XInput) kullanılan oyunlarda Windows bunu "giriş" saymaz, oturum 5 dk sonra sıfırlanır.
"""
import ctypes
import time

IDLE_LIMIT_S = 300


class _LASTINPUTINFO(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]


def idle_seconds():
    """Son klavye/fare girişinden beri geçen süre (sn)."""
    lii = _LASTINPUTINFO()
    lii.cbSize = ctypes.sizeof(_LASTINPUTINFO)
    if not ctypes.windll.user32.GetLastInputInfo(ctypes.byref(lii)):
        return 0.0
    tick = ctypes.windll.kernel32.GetTickCount()
    return ((tick - lii.dwTime) & 0xFFFFFFFF) / 1000.0


class Oturum:
    def __init__(self):
        self.start = None

    def seconds(self):
        """Şu anki kesintisiz oturum süresi (sn)."""
        now = time.time()
        idle = idle_seconds()
        if idle >= IDLE_LIMIT_S:
            self.start = None                   # uzun süre hareketsiz: oturum bitti
            return 0
        if self.start is None:
            self.start = now - idle             # oturum, son girişten itibaren başlar
        return int(now - self.start)
