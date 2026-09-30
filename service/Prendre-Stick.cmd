@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Prendre-Stick.ps1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Pause-Si-Humain.ps1"