@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-StickS3.ps1" -InstallStartup
echo.
pause
