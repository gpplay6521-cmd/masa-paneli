@echo off
rem Masa paneli yardimcisini kurar/gunceller ve Windows ile baslamasini saglar (yonetici izni gerekmez). Ayrintilar: kur.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0kur.ps1" %*
echo.
pause
