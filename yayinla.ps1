# Yeni panel yazılımı sürümünü yayınlar (bu bilgisayarda, ESP-IDF v6.1 kurulu olmalı):
#   1) pc-helper\paketle.ps1 -Hafif   panelin bilgisayara indirttiği hafif kurulum dosyasını tazeler (panel yazılımına gömülür)
#   2) idf.py build                   panel yazılımını derler (gömülü kurulum dosyası içinde)
#   3) pc-helper\paketle.ps1          firmware\ paketini, MasaPaneli-Yardimci.zip ve MasaPaneli-Kurulum.cmd'yi üretir
# ÖNCESİNDE panel-firmware\main\fw_version.h'daki sürümü ARTIR. Sonra bu bilgisayardaki yardımcı program, panel bağlanınca paketi paneldekinden
# yeni bulup paneli kendiliğinden günceller (ya da elle: pc-helper\panel_yukle.bat).
# Kullanım:  powershell -ExecutionPolicy Bypass -File yayinla.ps1
$ErrorActionPreference = 'Stop'
$Kok = $PSScriptRoot
$env:MSYSTEM = $null
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1 | Out-Null

$surum = [regex]::Match((Get-Content "$Kok\panel-firmware\main\fw_version.h" -Raw), 'FW_VERSION\s+"([0-9.]+)"').Groups[1].Value
Write-Host "== Yayinlanacak panel yazilimi surumu: $surum =="

powershell -NoProfile -ExecutionPolicy Bypass -File "$Kok\pc-helper\paketle.ps1" -Hafif
if ($LASTEXITCODE -ne 0) { throw 'Hafif kurulum hazirlanamadi.' }

Push-Location "$Kok\panel-firmware"
try {
    idf.py build
    if ($LASTEXITCODE -ne 0) { throw 'Panel yazilimi derlenemedi.' }
} finally { Pop-Location }

powershell -NoProfile -ExecutionPolicy Bypass -File "$Kok\pc-helper\paketle.ps1"
if ($LASTEXITCODE -ne 0) { throw 'Tam paket hazirlanamadi.' }
Write-Host "== Tamam: surum $surum. Bu bilgisayardaki yardimci panel baglaninca eski sürümlü paneli kendiliginden gunceller. =="
