@echo off
rem Panel yazilimini (pc-helper\firmware) panele yukler; ESP-IDF gerekmez. Ayrintilar ve secenekler: panel_yukle.py
rem   panel_yukle.bat              panel eskiyse guncelle     panel_yukle.bat --kontrol   yalnizca surumleri goster
rem   panel_yukle.bat --zorla      surum ne olursa olsun yukle
chcp 65001 >nul
if not exist "%~dp0.venv\Scripts\python.exe" (
  echo Once kur.bat calistirilmali ^(Python ortami yok^).
  pause
  exit /b 1
)
"%~dp0.venv\Scripts\python.exe" -X utf8 "%~dp0panel_yukle.py" %*
echo.
pause
