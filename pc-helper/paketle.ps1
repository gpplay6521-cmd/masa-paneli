# Yardımcı klasöründen taşınabilir kurulum paketleri yapar.
#
#   paketle.ps1          TAM paket (proje kökünde):
#                          MasaPaneli-Kurulum.cmd   TEK DOSYA: başka bir bilgisayarda çift tıklanır; paketi %LOCALAPPDATA%\MasaPaneli\pc-helper'a açar ve
#                                                   kur.ps1'i çalıştırır (Python + paketler + panel yazılımı denetimi/yüklemesi + Windows ile başlatma).
#                          MasaPaneli-Yardimci.zip  aynı içerik zip olarak
#                        İçerik: .venv, önbellek, ekran görüntüleri, kapak kayıtları, günlükler HARİÇ; panel yazılımı (firmware\) DAHİL.
#                        firmware\ paketi panel-firmware\build'ten tazelenir (derleme yoksa mevcut paket kullanılır).
#   paketle.ps1 -Hafif   HAFİF kurulum: panel yazılımı paketi (firmware\) OLMADAN, panel-firmware\main\install\MasaPaneli-Kurulum.cmd olarak yazılır.
#                        Panel bu dosyayı yazılımının içinde taşır ve kendi Wi-Fi ağından bilgisayara indirtir ("Bu bilgisayara kur").
#                        Panelin zaten kendi yazılımı olduğu için firmware\ gerekmez (kendi kopyasını gömmesi de sonsuz döngü olurdu).
#
# Yeni sürüm yayınlama sırası (yayinla.ps1 hepsini yapar):  fw_version.h'ı artır -> paketle.ps1 -Hafif -> idf.py build -> paketle.ps1
param([switch]$Hafif)
$ErrorActionPreference = 'Stop'
$Klasor = $PSScriptRoot
$Kok = Split-Path $Klasor -Parent
$VenvPy = Join-Path $Klasor '.venv\Scripts\python.exe'

# --- tek dosyalık kurulum: cmd başlığı + PowerShell kodu + base64 zip yükü (cmd, "exit /b"den sonrasını okumaz) ---
function Yap-Cmd($zipYolu, $cmdYolu) {
    $b1 = "#PS-" + "BASLA"          # işaretleri parçalı yazıyoruz: komut satırındaki metin işareti taklit etmesin
    $b2 = "#PAYLOAD-" + "BASLA"
    $baslik = @(
        '@echo off',
        'rem Masa Paneli - tek dosyalik kurulum. Cift tikla: paketi %LOCALAPPDATA%\MasaPaneli\pc-helper klasorune acar ve kurulumu calistirir.',
        'rem Yonetici izni gerekmez. Ek secenekler ornegin: MasaPaneli-Kurulum.cmd -YazilimYok   (kur.ps1 secenekleri)',
        'setlocal',
        'set "MP_DOSYA=%~f0"',
        'set "MP_ARGS=%*"',
        'powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=[IO.File]::ReadAllText($env:MP_DOSYA); $i=$s.IndexOf(''#PS-''+''BASLA'')+9; $j=$s.IndexOf(''#PAYLOAD-''+''BASLA''); Invoke-Expression $s.Substring($i,$j-$i)"',
        'set "MP_SON=%errorlevel%"',
        'echo.',
        'pause',
        'exit /b %MP_SON%'
    ) -join "`r`n"
    $ps = @'
$ErrorActionPreference = 'Stop'
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch { }
$hedef = $env:MASAPANELI_HEDEF
if (-not $hedef) { $hedef = Join-Path $env:LOCALAPPDATA 'MasaPaneli' }
Write-Host "== Masa Paneli kurulumu -> $hedef =="
Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='pythonw.exe'" | Where-Object { $_.CommandLine -like '*panel_helper.py*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 800
$b64 = (($s.Substring($j) -split "`r?`n") | Select-Object -Skip 1 | ForEach-Object { $_.Trim() }) -join ''
$zip = Join-Path $env:TEMP ('masapaneli_' + [guid]::NewGuid().ToString('N') + '.zip')
$tmp = Join-Path $env:TEMP ('masapaneli_' + [guid]::NewGuid().ToString('N'))
[IO.File]::WriteAllBytes($zip, [Convert]::FromBase64String($b64))
Expand-Archive -Path $zip -DestinationPath $tmp -Force
$kaynak = Join-Path $tmp 'pc-helper'
$hedefKlasor = Join-Path $hedef 'pc-helper'
$null = New-Item -ItemType Directory -Path $hedefKlasor -Force
# guncelleme: kullanicinin ayar dosyasi (panel_ayarlari.json) varsa ezilmez
Get-ChildItem $kaynak -Force | ForEach-Object {
    if ($_.Name -eq 'panel_ayarlari.json' -and (Test-Path (Join-Path $hedefKlasor $_.Name))) { return }
    Copy-Item -Path $_.FullName -Destination $hedefKlasor -Recurse -Force
}
Remove-Item $zip, $tmp -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'Dosyalar acildi; kurulum basliyor...'
$kurArgs = @()
if ($env:MP_ARGS) { $kurArgs = @($env:MP_ARGS -split '\s+' | Where-Object { $_ }) }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $hedefKlasor 'kur.ps1') @kurArgs
exit $LASTEXITCODE
'@
    $yuk = [Convert]::ToBase64String([IO.File]::ReadAllBytes($zipYolu), [Base64FormattingOptions]::InsertLineBreaks)
    $icerik = $baslik + "`r`n" + $b1 + "`r`n" + ($ps -replace "`r?`n", "`r`n") + "`r`n" + $b2 + "`r`n" + $yuk + "`r`n"
    [IO.File]::WriteAllText($cmdYolu, $icerik, (New-Object System.Text.ASCIIEncoding))
}

# --- pc-helper'ı geçici klasöre kopyalayıp zip yap ($haric: atlanacak klasörler) ---
function Yap-Zip($zipYolu, $haric) {
    $gecici = Join-Path $env:TEMP ('masapaneli_' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path "$gecici\pc-helper"
    robocopy $Klasor "$gecici\pc-helper" /E /XD $haric /XF panel_helper_gunluk.txt panel_helper_gunluk.eski.txt *.pyc /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy hatasi: $LASTEXITCODE" }
    if (Test-Path $zipYolu) { Remove-Item $zipYolu -Force }
    Compress-Archive -Path "$gecici\pc-helper" -DestinationPath $zipYolu
    Remove-Item $gecici -Recurse -Force
}

if ($Hafif) {
    $hedefDir = Join-Path $Kok 'panel-firmware\main\install'
    $null = New-Item -ItemType Directory -Path $hedefDir -Force
    $zip = Join-Path $env:TEMP ('masapaneli_hafif_' + [guid]::NewGuid().ToString('N') + '.zip')
    Yap-Zip $zip @('.venv', '__pycache__', 'shots', 'kapak_son', 'firmware')
    $cmd = Join-Path $hedefDir 'MasaPaneli-Kurulum.cmd'
    Yap-Cmd $zip $cmd
    Remove-Item $zip -Force
    Write-Host ("Hafif kurulum hazir: {0} ({1:N0} KB) - panel yazilimina gomulecek (idf.py build)" -f $cmd, ((Get-Item $cmd).Length / 1KB))
    exit 0
}

if ((Test-Path (Join-Path $Kok 'panel-firmware\build\flasher_args.json')) -and (Test-Path $VenvPy)) {
    & $VenvPy -X utf8 (Join-Path $Klasor 'firmware_hazirla.py')
    if ($LASTEXITCODE -ne 0) { throw 'Panel yazilimi paketi hazirlanamadi (firmware_hazirla.py); once panel yazilimini derle: idf.py build' }
} else {
    Write-Host 'Uyari: panel-firmware\build yok; mevcut firmware\ paketi kullaniliyor.'
}
if (-not (Test-Path (Join-Path $Klasor 'firmware\manifest.json'))) { throw 'firmware\manifest.json yok: panel yazilimi paketi olmadan kurulum paketi yapilmaz.' }

$Zip = Join-Path $Kok 'MasaPaneli-Yardimci.zip'
$Cmd = Join-Path $Kok 'MasaPaneli-Kurulum.cmd'
Yap-Zip $Zip @('.venv', '__pycache__', 'shots', 'kapak_son')
Write-Host ("Zip hazir: {0} ({1:N0} KB)" -f $Zip, ((Get-Item $Zip).Length / 1KB))
Yap-Cmd $Zip $Cmd
Write-Host ("Tek dosya kurulum hazir: {0} ({1:N0} KB)" -f $Cmd, ((Get-Item $Cmd).Length / 1KB))
