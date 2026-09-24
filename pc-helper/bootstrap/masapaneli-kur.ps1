# Masa Paneli - USB uzerinden ilk kurulum (Wi-Fi'si olmayan bilgisayarlar icin)
# Panelin USB kablosuyla bagli oldugu bu bilgisayarda calistirilir; panelden kurulum dosyasini
# USB-seri baglanti uzerinden alir, dogrular ve kurulumu kendiliginden baslatir.
#
# BU DOSYA PANELE GOMULMEZ: panel ekraninda gosterilen tek satirlik komut (strings_tr.h,
# TR_INSTALL_USB_SCRIPT), bu dosyanin yayinlandigi GitHub Gist raw URL'sini "irm ... | iex" ile cagirir.
# Burada degisiklik yaptiktan sonra ayni gist'e yeniden yayinlamak icin:
#   gh gist edit 79484443670b2ec5832c3813c982e733 pc-helper/bootstrap/masapaneli-kur.ps1
# (Gist URL'si degismez; panel yazilimini yeniden derlemeye gerek yoktur.)
$m = Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'CH340' }
$c = if ($m) { $m.Name -replace '.*\((COM\d+)\).*', '$1' } else { [IO.Ports.SerialPort]::GetPortNames()[0] }
Write-Host "Masa Paneli COM portu: $c"
$p = New-Object IO.Ports.SerialPort $c, 921600, 'None', 8, 'One'
$p.ReadBufferSize = 4194304
$p.NewLine = "`r`n"
$p.Open()
Start-Sleep -Seconds 2
$p.WriteLine('@KAL')

function Oku($f) {
    while ($true) {
        try { return (& $f) } catch [System.IO.IOException] { }
    }
}

Write-Host 'Kurulum dosyasi bekleniyor...'
do { $hdr = Oku { $p.ReadLine() } } while (-not $hdr.StartsWith('@KBAS'))
$hd = $hdr.Split("`t")
$len = [long]$hd[1]
$hash = $hd[2]

$out = "$env:TEMP\MasaPaneli-Kurulum.cmd"
$w = [IO.File]::Create($out)
$buf = New-Object byte[] 65536
$got = 0
while ($got -lt $len) {
    $n = Oku { $p.Read($buf, 0, [Math]::Min(65536, $len - $got)) }
    $w.Write($buf, 0, $n)
    $got += $n
    Write-Progress -Activity 'Masa Paneli kurulum dosyasi aliniyor' -Status "$got / $len bayt" -PercentComplete ([Math]::Min(100, 100 * $got / $len))
}
$w.Close()
$p.Close()
Write-Progress -Activity 'Masa Paneli kurulum dosyasi aliniyor' -Completed

if ((Get-FileHash $out -Algorithm SHA256).Hash -ieq $hash) {
    Write-Host 'Dogrulandi. Kurulum basliyor...'
    & $out
} else {
    Write-Host 'HATA: dosya bozuk geldi. Panel ekranindan "USB ile" secenegini tekrar sec ve bu komutu yeniden calistir.'
}
