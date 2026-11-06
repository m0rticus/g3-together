@echo off
rem G3 Together English setup.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*
echo.
pause
