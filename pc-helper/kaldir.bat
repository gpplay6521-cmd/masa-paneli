@echo off
rem Masa paneli yardimcisini durdurur ve Windows ile baslatmayi kaldirir.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0kur.ps1" -Kaldir
echo.
pause
