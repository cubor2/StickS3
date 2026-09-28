@echo off
setlocal
rem Cree un raccourci "Prendre-Stick" dans le menu Demarrer (Programs),
rem avec l'icone sticks3.ico. A relancer apres un git pull sur un nouveau PC.
set "STICKS3_SRC=%~dp0Prendre-Stick.cmd"
set "STICKS3_ICO=%~dp0sticks3.ico"
if not exist "%STICKS3_ICO%" (
    echo Erreur : sticks3.ico introuvable a cote de ce script.
    pause
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ws = New-Object -ComObject WScript.Shell; $lnkPath = Join-Path $ws.SpecialFolders('Programs') 'Prendre-Stick.lnk'; $lnk = $ws.CreateShortcut($lnkPath); $lnk.TargetPath = $env:STICKS3_SRC; $lnk.WorkingDirectory = Split-Path $env:STICKS3_SRC; $lnk.IconLocation = ($env:STICKS3_ICO + ',0'); $lnk.Description = 'Attache le StickS3 a ce PC'; $lnk.Save(); Write-Host ('Raccourci cree : ' + $lnkPath)"
if errorlevel 1 pause
