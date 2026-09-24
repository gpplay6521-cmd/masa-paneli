<#
Masa paneli yardımcı programını kurar / günceller. Yönetici izni GEREKMEZ (her şey bu kullanıcının hesabına kurulur).
  - Uygun Python'u bulur (yoksa Python 3.12'yi kullanıcı için kurar), bu klasörde .venv oluşturur, gereken paketleri yükler
  - Windows oturum açılışında kendiliğinden başlaması için kullanıcı kaydına ekler (HKCU\...\Run, "MasaPaneli")
  - Panel USB'de takılıysa panel yazılımını denetler; paketteki (firmware\) sürüm panelinkinden yeniyse ya da panel eski yazılımdaysa
    yeni yazılımı panele yükler (panel_yukle.py; yaklaşık 1 dk). Panel bulunamazsa/yanıt vermezse dokunmaz.
  - Yardımcıyı hemen başlatır (konsol penceresi açmadan, arka planda; günlük: panel_helper_gunluk.txt)

Kullanım (kur.bat üzerinden ya da doğrudan):
  kur.bat                     kur / güncelle + panel yazılımını denetle + başlat
  kur.bat -Durdur             çalışan yardımcıyı durdur (tanılama betikleri için)
  kur.bat -Kaldir             otomatik başlatmayı kaldır ve durdur
  kur.bat -OtomatikBaslatmaYok    Windows ile başlatma kaydı ekleme
  kur.bat -BaslatmaYok            kurulumdan sonra hemen başlatma
  kur.bat -YazilimYok             panel yazılımı denetimi/yüklemesini atla
  kur.bat -EklentiYok             YouTube Music kısayol eklentisi denetimini/yönlendirmesini atla
Bu klasör başka bir bilgisayara kopyalanırsa (paketle.ps1 zip / tek dosyalı kurulum yapar) orada yalnızca kur.bat çalıştırılır; bozuk/uyumsuz .venv otomatik yenilenir.
Chrome eklentisi (chrome-eklenti\) Chrome Web Store'da yayınlı DEĞİL (geliştirici hesabı/incelemesi gerektirir,
ve yerel WebSocket'e bağlanması incelemede sorun çıkarabilir); ayrıca sessiz/otomatik kurulum (ExtensionInstallForcelist)
yönetici izni ister ve bu betiğin "yönetici gerekmez" ilkesini bozar. Bunun yerine: kur.ps1 eklentinin o bilgisayarda
zaten yüklü olup olmadığını Chrome profilinden denetler, yüklü değilse tarayıcıyı açar/öne getirir, klasör yolunu
panoya kopyalar ve "chrome://extensions yaz" + "Geliştirici modu" + "Paketlenmemiş öğe yükle" adımlarını ekranda
gösterir (yönetici izni gerekmez). Not: chrome://extensions'ı komut satırından ZORLA açmak (Start-Process -ArgumentList
ya da pencereyi odaklayıp Ctrl+T + yazma simülasyonu) denendi, gerçek cihazda ikisi de güvenilmez çıktı (Chrome
"chrome://" adreslerini komut satırından güvenlik gereği yok sayıyor; klavye simülasyonu da Windows'un odak-çalma
engellemesi yüzünden bazen çalışmıyor) — bu yüzden adresi kullanıcının kendisi yazıyor, tek elle adım budur.
#>
param(
    [switch]$Kaldir,
    [switch]$Durdur,
    [switch]$OtomatikBaslatmaYok,
    [switch]$BaslatmaYok,
    [switch]$YazilimYok,
    [switch]$EklentiYok
)
$ErrorActionPreference = 'Stop'
$Klasor = $PSScriptRoot
$RunKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$RunAd = 'MasaPaneli'
$Venv = Join-Path $Klasor '.venv'
$VenvPy = Join-Path $Venv 'Scripts\python.exe'
$VenvPyW = Join-Path $Venv 'Scripts\pythonw.exe'
$Betik = Join-Path $Klasor 'panel_helper.py'
$PaketSinama = 'import serial, psutil, PIL, pynvml, esptool, winrt.windows.media.control, winrt.windows.ui.notifications, winrt.windows.data.xml.dom, winrt.windows.storage.streams'

function Yaz($m) { Write-Host $m }

function Yardimciyi-Durdur {
    $n = 0
    $liste = @(Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='pythonw.exe'" |
        Where-Object { $_.CommandLine -like '*panel_helper.py*' })
    foreach ($p in $liste) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue; $n++ }
    if ($n -gt 0) { Start-Sleep -Milliseconds 1000 }
    return $n
}

# Çalışıyor, 3.10-3.13 ve 64 bit ise gerçek python.exe yolunu döndürür (Microsoft Store'un boş "python" kısayolu elenir)
function Python-Sina($exe, $ekArgs) {
    $eski = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = @(& $exe @ekArgs -c "import sys,struct;print(sys.executable);print('%d.%d'%sys.version_info[:2]);print(struct.calcsize('P')*8)" 2>$null)
        if ($LASTEXITCODE -ne 0 -or $out.Count -lt 3) { return $null }
        $s = [version]([string]$out[1])
        if ([string]$out[2] -ne '64' -or $s -lt [version]'3.10' -or $s -gt [version]'3.13') { return $null }
        return [string]$out[0]
    } catch { return $null } finally { $ErrorActionPreference = $eski }
}

function Python-Bul {
    $adaylar = @(@('py', @('-3.12')), @('py', @('-3.13')), @('py', @('-3.11')), @('py', @('-3.10')), @('python', @()), @('python3', @()))
    foreach ($a in $adaylar) {
        if (Get-Command $a[0] -ErrorAction SilentlyContinue) {
            $p = Python-Sina $a[0] $a[1]
            if ($p) { return $p }
        }
    }
    $yollar = @()
    $yollar += @(Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" -ErrorAction SilentlyContinue)
    $yollar += @(Get-ChildItem "$env:ProgramFiles\Python3*\python.exe" -ErrorAction SilentlyContinue)
    foreach ($y in ($yollar | Sort-Object FullName -Descending)) {
        $p = Python-Sina $y.FullName @()
        if ($p) { return $p }
    }
    return $null
}

function Python-Kur {
    Yaz 'Python bulunamadi; Python 3.12 kuruluyor (yalnizca bu kullanici icin, yonetici izni gerekmez)...'
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        try {
            & winget install --id Python.Python.3.12 -e --scope user --silent --accept-package-agreements --accept-source-agreements | Out-Host
        } catch { }
        $p = Python-Bul
        if ($p) { return $p }
    }
    $dosya = Join-Path $env:TEMP 'python-3.12.10-amd64.exe'
    Invoke-WebRequest -Uri 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe' -OutFile $dosya -UseBasicParsing
    Start-Process -FilePath $dosya -ArgumentList '/quiet', 'InstallAllUsers=0', 'PrependPath=0', 'Include_test=0', 'Include_launcher=0' -Wait
    return (Python-Bul)
}

function Venv-Saglam {
    if (-not (Test-Path $VenvPy)) { return $false }
    $eski = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $VenvPy -c $PaketSinama 2>$null
        return ($LASTEXITCODE -eq 0)
    } catch { return $false } finally { $ErrorActionPreference = $eski }
}

# Chrome profillerinden birinde bu klasor yola sahip paketlenmemis (Gelistirici modu) bir eklenti kayitli mi?
function Eklenti-Kurulu($eklentiYolu) {
    $kokler = @(
        (Join-Path $env:LOCALAPPDATA 'Google\Chrome\User Data'),
        (Join-Path $env:LOCALAPPDATA 'Microsoft\Edge\User Data')
    )
    $aranan = ($eklentiYolu -replace '\\', '\\')
    foreach ($kok in $kokler) {
        if (-not (Test-Path $kok)) { continue }
        $tercihDosyalari = @(Get-ChildItem $kok -Filter 'Preferences' -Recurse -Depth 1 -ErrorAction SilentlyContinue) +
                           @(Get-ChildItem $kok -Filter 'Secure Preferences' -Recurse -Depth 1 -ErrorAction SilentlyContinue)
        foreach ($dosya in $tercihDosyalari) {
            try {
                $icerik = Get-Content -Raw -LiteralPath $dosya.FullName -ErrorAction Stop
            } catch { continue }
            if ($icerik -like "*$aranan*") { return $true }
        }
    }
    return $false
}

function Tarayiciyi-One-Getir($tarayiciYolu) {
    # Chrome/Edge, guvenlik nedeniyle "chrome://..." adreslerini komut satirindan (Start-Process -ArgumentList) kabul etmez;
    # zaten acik bir tarayiciya boyle bir istek gonderilince adres yok sayilir, sadece bos bir sekme acilir (gercek cihazda
    # dogrulandi). Bunun yerine burada EN IYI GAYRET ile klavye simulasyonu denenir (pencereyi odakla + Ctrl+T + yaz);
    # basarisiz olursa sessizce gecilir CUNKU ekrandaki talimat zaten kullaniciya "chrome://extensions yaz" diyor - bu
    # fonksiyon basarili olursa o adimi atlatir (bonus), basarisiz olursa hicbir sey bozulmaz.
    $procAdi = [IO.Path]::GetFileNameWithoutExtension($tarayiciYolu)
    $pencere = @(Get-Process -Name $procAdi -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }) | Select-Object -First 1
    if (-not $pencere) {
        Start-Process -FilePath $tarayiciYolu
        for ($i = 0; $i -lt 20 -and -not $pencere; $i++) {
            Start-Sleep -Milliseconds 300
            $pencere = @(Get-Process -Name $procAdi -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }) | Select-Object -First 1
        }
        return   # yeni acilan pencere zaten bos sekmede; klavye simulasyonuna gerek yok, kullanici adresi yazacak
    }
    try {
        $wshell = New-Object -ComObject WScript.Shell
        $odaklandi = $wshell.AppActivate($pencere.Id)
        if (-not $odaklandi) { Start-Sleep -Milliseconds 400; $odaklandi = $wshell.AppActivate($pencere.Id) }
        if ($odaklandi) {
            Start-Sleep -Milliseconds 400
            $wshell.SendKeys('^t')
            Start-Sleep -Milliseconds 500
            $wshell.SendKeys('chrome://extensions/{ENTER}')
        }
    } catch { }
}

function Tarayici-Yolu-Bul {
    foreach ($ad in @('chrome.exe', 'msedge.exe')) {
        $cmd = Get-Command $ad -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    $adaylar = @(
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
        "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe",
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
    )
    foreach ($a in $adaylar) { if (Test-Path $a) { return $a } }
    return $null
}

if ($Durdur) {
    $n = Yardimciyi-Durdur
    Yaz ("Durduruldu ({0} surec)." -f $n)
    exit 0
}
if ($Kaldir) {
    $null = Yardimciyi-Durdur
    Remove-ItemProperty -Path $RunKey -Name $RunAd -ErrorAction SilentlyContinue
    Remove-Item 'HKCU:\Software\Classes\AppUserModelId\MasaPaneli.Yardimci' -Recurse -ErrorAction SilentlyContinue
    Yaz 'Otomatik baslatma kaldirildi ve yardimci durduruldu. Bu klasoru silebilirsin.'
    exit 0
}

try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch { }
Yaz '== Masa paneli yardimcisi kurulumu =='
$null = Yardimciyi-Durdur

if (Venv-Saglam) {
    Yaz 'Python ortami hazir.'
} else {
    $py = Python-Bul
    if (-not $py) { $py = Python-Kur }
    if (-not $py) { throw 'Python bulunamadi ve kurulamadi. python.org adresinden Python 3.12 (64 bit) kurup bu betigi yeniden calistir.' }
    Yaz "Python: $py"
    if (Test-Path $Venv) { Remove-Item $Venv -Recurse -Force }
    & $py -m venv $Venv
    if ($LASTEXITCODE -ne 0) { throw 'Sanal ortam olusturulamadi.' }
    Yaz 'Paketler yukleniyor (internet gerekir, birkac dakika surebilir)...'
    & $VenvPy -m pip install --disable-pip-version-check --quiet -r (Join-Path $Klasor 'requirements.txt')
    if ($LASTEXITCODE -ne 0) { throw 'Paketler yuklenemedi (internet baglantisini denetle).' }
    if (-not (Venv-Saglam)) { throw 'Kurulum sonrasi paket denetimi basarisiz.' }
    Yaz 'Paketler yuklendi.'
}

if (-not $YazilimYok) {
    Yaz 'Panel yazilimi denetleniyor (panel USB ile takili olmali)...'
    try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
    $eski = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $VenvPy -X utf8 (Join-Path $Klasor 'panel_yukle.py') --otomatik --yardimciya-dokunma
    $kod = $LASTEXITCODE
    $ErrorActionPreference = $eski
    if ($kod -eq 1) { Yaz 'UYARI: panel yazilimi yuklenemedi (ayrinti yukarida). Yardimci yine de kurulacak; sonra "panel_yukle.bat" ile tekrar dene.' }
    elseif ($kod -eq 2) { Yaz 'Not: panel takili degil; yazilim denetimi atlandi. Panel taktiginda yardimci program surumu kendisi denetler.' }
}

$EklentiKlasoru = Join-Path $Klasor 'chrome-eklenti'
if (-not $EklentiYok -and (Test-Path $EklentiKlasoru)) {
    if (Eklenti-Kurulu $EklentiKlasoru) {
        Yaz 'YouTube Music kisayol eklentisi zaten kurulu.'
    } else {
        Yaz ''
        Yaz '== YouTube Music kisayol eklentisi (Begen/Karistir/Tekrarla) kurulu degil =='
        Yaz 'Tarayicida adres cubuguna "chrome://extensions" yaz ve Enter''a bas. Orada:'
        Yaz '  1) Sag ustten "Gelistirici modu" anahtarini ac'
        Yaz '  2) "Paketlenmemis oge yukle" düğmesine tıkla'
        Yaz "  3) Su klasoru sec (yolu panoya kopyalandi, adres cubuguna yapistirabilirsin):"
        Yaz "     $EklentiKlasoru"
        try { Set-Clipboard -Value $EklentiKlasoru } catch { }
        $tarayici = Tarayici-Yolu-Bul
        if ($tarayici) {
            Tarayiciyi-One-Getir $tarayici
        } else {
            Yaz 'Not: Chrome/Edge bulunamadi; tarayicini acip adres cubuguna chrome://extensions yaz.'
        }
    }
}

if (-not $OtomatikBaslatmaYok) {
    $komut = '"{0}" -X utf8 "{1}"' -f $VenvPyW, $Betik
    Set-ItemProperty -Path $RunKey -Name $RunAd -Value $komut
    Yaz 'Windows acilisinda otomatik baslama: ACIK'
}

if (-not $BaslatmaYok) {
    Start-Process -FilePath $VenvPyW -ArgumentList @('-X', 'utf8', ('"{0}"' -f $Betik)) -WorkingDirectory $Klasor -WindowStyle Hidden
    Start-Sleep -Seconds 4
    $calisiyor = @(Get-CimInstance Win32_Process -Filter "Name='pythonw.exe'" | Where-Object { $_.CommandLine -like '*panel_helper.py*' }).Count -gt 0
    if ($calisiyor) { Yaz 'Yardimci baslatildi (arka planda). Gunluk: panel_helper_gunluk.txt' }
    else { Yaz 'UYARI: yardimci baslamadi; panel_helper_gunluk.txt dosyasina bak.' }
}

# Panel USB'de görünüyor mu? (CH340K, VID 1A86)
$ch = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_1A86*' })
if ($ch.Count -eq 0) {
    Yaz 'Not: panel USB''de gorunmuyor. Kablo VERI kablosu olmali (yalnizca sarj kablosu calismaz); panelin USB-UART (CH340) portuna tak.'
} elseif (@($ch | Where-Object { $_.Status -ne 'OK' }).Count -gt 0) {
    Yaz 'Not: panel gorunuyor ama surucusu tam yuklenmemis. Windows Update ile gelmezse WCH CH340 surucusunu (wch-ic.com) kur.'
} else {
    Yaz 'Panel USB''de gorunuyor.'
}
Yaz 'Tamam.'

