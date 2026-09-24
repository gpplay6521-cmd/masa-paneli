"""Konum: panelin takılı olduğu bilgisayarın yaklaşık yerini bulup MGM il/ilçesine çevirir.

Yöntem: bilgisayarın internet bağlantısının IP adresinden konum (ipinfo.io, HTTPS, anahtarsız). Windows'un kendi konum servisi bu
bilgisayarda kapalı (Geolocator "NoData"), o yüzden kullanılamadı. IP ile konum kaba ve yanıltıcı olabilir: bu bağlantıda
ipinfo.io "İzmir", ipapi.co "İstanbul" dedi (iki servis çelişti); ipapi.co güvenilmez bulunduğu için kullanılmıyor.
Bulunamazsa (ağ yok, Türkiye dışı, MGM'de eşleşmezse) aşağıdaki DEFAULT_IL/DEFAULT_ILCE kullanılır (kendi ilin/ilçenle değiştir).
IP ile yalnızca il bulunursa: bulunan il, varsayılan ille aynıysa ilçe olarak varsayılan ilçe kullanılır;
IP servisi ilçe düzeyinde bir şehir verirse (ör. "Bornova") o ilçe kullanılır; başka ilde il merkezi kullanılır.
Kesin konum için panel_ayarlari.json'a "il" ve "ilce" yazılır (otomatik bulmanın yerini alır).
"""
import json
import urllib.request

import weather

DEFAULT_IL, DEFAULT_ILCE = "Ankara", "Çankaya"       # yalnızca IP ile konum bulunamazsa kullanılan yedek; kendi ilin/ilçenle değiştirebilirsin
IP_SERVICE = "https://ipinfo.io/json"


def _ip_lookup():
    req = urllib.request.Request(IP_SERVICE, headers={"User-Agent": "masa-paneli"})
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.loads(r.read().decode("utf-8"))


def _strip_region(s):
    s = weather.fold(s)
    for suffix in (" province", " ili", " il"):
        if s.endswith(suffix):
            s = s[: -len(suffix)].strip()
    return s


def auto():
    """(il, ilce, açıklama, kesin). Hata fırlatmaz; bulunamazsa varsayılanı döndürür.
    kesin=False: ağ/servis hatası yüzünden varsayılana düşüldü (geçici; kısa süre sonra yeniden denenmeli, örn. bilgisayar açılırken ağ
    henüz hazır değildir); kesin=True: sonuç kalıcı (bulundu ya da Türkiye dışı/eşleşmedi)."""
    default = (DEFAULT_IL, DEFAULT_ILCE)
    try:
        d = _ip_lookup()
    except Exception as e:  # noqa: BLE001
        return (*default, f"konum servisine ulaşılamadı ({e}); varsayılan konum", False)
    if (d.get("country") or "").upper() != "TR":
        return (*default, f"IP Türkiye dışı görünüyor ({d.get('country')}); varsayılan konum", True)
    try:
        iller = [r["il"] for r in weather.list_iller()]
        by_fold = {weather.fold(n): n for n in iller}
        il = by_fold.get(_strip_region(d.get("region"))) or by_fold.get(weather.fold(d.get("city")))
        if not il:
            return (*default, f"'{d.get('region')}' MGM illerinde bulunamadı; varsayılan konum", True)
        ilce = ""
        city = weather.fold(d.get("city"))
        if city and city != weather.fold(il):
            for r in weather.list_ilceler(il):
                if weather.fold(r.get("ilce")) == city:
                    ilce = r["ilce"]
                    break
        if not ilce and weather.fold(il) == weather.fold(DEFAULT_IL):
            ilce = DEFAULT_ILCE
        return il, ilce, f"IP konumu ({IP_SERVICE.split('/')[2]}): {d.get('city')}, {d.get('region')}", True
    except Exception as e:  # noqa: BLE001
        return (*default, f"MGM il listesi alınamadı ({e}); varsayılan konum", False)
