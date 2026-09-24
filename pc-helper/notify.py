"""Windows bildirimi (toast): panelde alarm çalınca bilgisayarda da haber verir.

Kayıtsız (paketsiz) bir uygulamanın bildirim gösterebilmesi için bir uygulama kimliği (AUMID) gerekir. Kimlik ve görünen ad,
kullanıcı kaydına (HKCU\\Software\\Classes\\AppUserModelId\\MasaPaneli.Yardimci, DisplayName="Masa Paneli") yazılır; yönetici izni
gerekmez, silmek için bu anahtarı kaldırmak yeterli. Bildirim sesi Windows'un "Hatırlatıcı" sesidir.
"""
import winreg
from xml.sax.saxutils import escape

from winrt.windows.data.xml.dom import XmlDocument
from winrt.windows.ui.notifications import ToastNotification, ToastNotificationManager

AUMID = "MasaPaneli.Yardimci"
APP_NAME = "Masa Paneli"
_registered = False


def _register():
    global _registered
    if _registered:
        return
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, rf"Software\Classes\AppUserModelId\{AUMID}") as k:
        winreg.SetValueEx(k, "DisplayName", 0, winreg.REG_SZ, APP_NAME)
    _registered = True


def show(title, body, persistent=True):
    """Bildirimi gönderir; Windows'un bu uygulama için bildirim ayarını (metin) döndürür.
    persistent=True: kapatılana kadar ekranda kalan hatırlatıcı (zamanlayıcı alarmı). False: normal bildirim, birkaç saniye görünüp
    kaybolur (mola hatırlatıcı; oyunda rahatsız etmesin) ve Eylem Merkezi'nde durur."""
    _register()
    xml = XmlDocument()
    if persistent:
        xml.load_xml(
            '<toast scenario="reminder"><visual><binding template="ToastGeneric">'
            f"<text>{escape(title)}</text><text>{escape(body)}</text>"
            "</binding></visual>"
            '<actions><action content="Tamam" arguments="dismiss" activationType="system"/></actions>'
            '<audio src="ms-winsoundevent:Notification.Reminder"/></toast>')
    else:
        xml.load_xml(
            '<toast><visual><binding template="ToastGeneric">'
            f"<text>{escape(title)}</text><text>{escape(body)}</text>"
            "</binding></visual></toast>")
    notifier = ToastNotificationManager.create_toast_notifier_with_id(AUMID)
    notifier.show(ToastNotification(xml))
    return str(notifier.setting)
