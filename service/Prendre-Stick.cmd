@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Prendre-Stick.ps1"
if errorlevel 1 pause
