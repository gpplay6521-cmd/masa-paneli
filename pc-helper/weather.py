"""Hava durumu: Meteoroloji Genel Müdürlüğü (MGM).

mgm.gov.tr sayfalarının kendi kullandığı servis (servis.mgm.gov.tr/web) okunur; bu servisin resmi bir API belgesi yoktur,
istekler sayfanın gönderdiği Origin/Referer başlıklarıyla yapılır. Uç noktalar (bu bilgisayardan gerçek çağrılarla doğrulandı):
  merkezler?il=<il>                 il merkezi (il adı; Türkçe karakterli olabilir)
  merkezler/ililcesi?il=<il>        ilin ilçe merkezleri
  sondurumlar?merkezid=<merkezId>   son gözlem (sıcaklık, hissedilen, nem, hadiseKodu)
  tahminler/gunluk?istno=<gunlukTahminIstNo>   5 günlük tahmin (bugünün en düşük/en yüksek: enDusukGun0/enYuksekGun0)
  tahminler/saatlik?istno=<saatlikTahminIstNo> saatlik tahmin (gözlem eskiyse yedek olarak)
Hadise kodlarının Türkçe adları panel yazılımındadır (MGM sayfa betiğindeki tablo).
"""
import json
import math
import urllib.parse
import urllib.request
from datetime import datetime, timezone

BASE = "https://servis.mgm.gov.tr/web"
HEADERS = {
    "Origin": "https://www.mgm.gov.tr",
    "Referer": "https://www.mgm.gov.tr/",
    "User-Agent": "Mozilla/5.0 (masa-paneli)",
}
NA = -999
STALE_OBS_HOURS = 3      # gözlem bundan eskiyse saatlik tahmine başvurulur


def _get(path, **params):
    url = f"{BASE}/{path}"
    if params:
        url += "?" + urllib.parse.urlencode(params, encoding="utf-8")
    req = urllib.request.Request(url, headers=HEADERS)
    with urllib.request.urlopen(req, timeout=12) as r:
        return json.loads(r.read().decode("utf-8"))


_FOLD = str.maketrans({"İ": "i", "I": "i", "ı": "i", "Ç": "c", "ç": "c", "Ğ": "g", "ğ": "g",
                       "Ö": "o", "ö": "o", "Ş": "s", "ş": "s", "Ü": "u", "ü": "u"})


def fold(s):
    """Türkçe harfleri ASCII karşılığına indirip küçült: 'Keçiören' == 'kecioren' == 'KECİÖREN'."""
    return (s or "").translate(_FOLD).lower().strip()


def list_iller():
    """MGM'nin il merkezleri (81 il)."""
    return _get("merkezler/iller")


def list_ilceler(il):
    """Bir ilin MGM ilçe merkezleri."""
    return _get("merkezler/ililcesi", il=il)


def resolve(il, ilce=""):
    """İl (ve isteğe bağlı ilçe) için MGM merkez bilgisini bul. Bulunamazsa ValueError."""
    if not il:
        raise ValueError("il belirtilmedi")
    if ilce:
        rows = _get("merkezler/ililcesi", il=il)
        hit = [r for r in rows if fold(r.get("ilce")) == fold(ilce)]
        if not hit:
            names = ", ".join(sorted({r.get("ilce", "") for r in rows}))
            raise ValueError(f"'{il}' ilinde '{ilce}' ilçesi yok. Seçenekler: {names}")
        row = hit[0]
        place = f"{row['ilce']}, {row['il']}"
    else:
        rows = _get("merkezler", il=il)
        if not rows:
            raise ValueError(f"'{il}' ili bulunamadı")
        row = next((r for r in rows if r.get("oncelik") == 1), rows[0])
        place = row["il"]
    return {
        "merkez_id": row["merkezId"],
        "gunluk_no": row.get("gunlukTahminIstNo"),
        "saatlik_no": row.get("saatlikTahminIstNo"),
        "lat": row.get("enlem"),
        "lon": row.get("boylam"),
        "place": place,
    }


def sun_elevation(lat, lon, when_utc):
    """Güneşin ufuk üstündeki yüksekliği (derece); NOAA/ESRL güneş hesabı (mgm.gov.tr'nin gün doğumu/batımı betiğiyle aynı yöntem)."""
    rad, deg = math.radians, math.degrees
    jd = when_utc.timestamp() / 86400.0 + 2440587.5
    t = (jd - 2451545.0) / 36525.0
    l0 = (280.46646 + t * (36000.76983 + t * 0.0003032)) % 360
    m = 357.52911 + t * (35999.05029 - 0.0001537 * t)
    e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t)
    c = (math.sin(rad(m)) * (1.914602 - t * (0.004817 + 0.000014 * t))
         + math.sin(rad(2 * m)) * (0.019993 - 0.000101 * t) + math.sin(rad(3 * m)) * 0.000289)
    omega = 125.04 - 1934.136 * t
    lam = l0 + c - 0.00569 - 0.00478 * math.sin(rad(omega))
    eps0 = 23 + (26 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60) / 60
    eps = eps0 + 0.00256 * math.cos(rad(omega))
    decl = math.asin(math.sin(rad(eps)) * math.sin(rad(lam)))
    y = math.tan(rad(eps / 2)) ** 2
    eqt = 4 * deg(y * math.sin(2 * rad(l0)) - 2 * e * math.sin(rad(m))
                  + 4 * e * y * math.sin(rad(m)) * math.cos(2 * rad(l0))
                  - 0.5 * y * y * math.sin(4 * rad(l0)) - 1.25 * e * e * math.sin(2 * rad(m)))
    minutes = when_utc.hour * 60 + when_utc.minute + when_utc.second / 60
    tst = (minutes + eqt + 4 * lon) % 1440
    ha = tst / 4 - 180
    if ha < -180:
        ha += 360
    cosz = (math.sin(rad(lat)) * math.sin(decl)
            + math.cos(rad(lat)) * math.cos(decl) * math.cos(rad(ha)))
    return 90 - deg(math.acos(max(-1.0, min(1.0, cosz))))


def is_night(lat, lon, when_utc=None):
    if lat is None or lon is None:
        return False
    return sun_elevation(lat, lon, when_utc or datetime.now(timezone.utc)) < -0.833


def _num(v):
    """MGM eksik değeri -9999 yazar."""
    if v is None or v <= -9000:
        return NA
    return int(round(v))


def _parse_time(s):
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00"))
    except Exception:  # noqa: BLE001
        return None


def fetch(loc):
    """Şimdiki hava durumu: dict(code, temp, feels, hum, tmin, tmax, night, place). Ağ hatasında istisna fırlatır."""
    now = datetime.now(timezone.utc)
    obs = (_get("sondurumlar", merkezid=loc["merkez_id"]) or [{}])[0]
    code = obs.get("hadiseKodu") or ""
    temp, feels, hum = _num(obs.get("sicaklik")), _num(obs.get("hissedilenSicaklik")), _num(obs.get("nem"))
    ts = _parse_time(obs.get("veriZamani") or "")
    stale = ts is None or (now - ts).total_seconds() > STALE_OBS_HOURS * 3600
    tmin = tmax = NA
    daily = None
    if loc.get("gunluk_no"):
        try:
            daily = (_get("tahminler/gunluk", istno=loc["gunluk_no"]) or [{}])[0]
            tmin, tmax = _num(daily.get("enDusukGun0")), _num(daily.get("enYuksekGun0"))
        except Exception:  # noqa: BLE001
            daily = None
    if (stale or temp == NA or not code) and loc.get("saatlik_no"):
        try:      # gözlem eski/eksik: en yakın saatlik tahmin
            hourly = (_get("tahminler/saatlik", istno=loc["saatlik_no"]) or [{}])[0].get("tahmin") or []
            if hourly:
                h = hourly[0]
                temp = _num(h.get("sicaklik")) if temp == NA or stale else temp
                feels = _num(h.get("hissedilenSicaklik")) if feels == NA or stale else feels
                hum = _num(h.get("nem")) if hum == NA or stale else hum
                code = h.get("hadise") or code
        except Exception:  # noqa: BLE001
            pass
    if not code and daily:
        code = daily.get("hadiseGun0") or ""
    return {"code": code, "temp": temp, "feels": feels, "hum": hum, "tmin": tmin, "tmax": tmax,
            "night": is_night(loc.get("lat"), loc.get("lon"), now), "place": loc["place"]}
