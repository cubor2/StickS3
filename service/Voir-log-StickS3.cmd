@echo off
setlocal
rem Affiche les logs du service lance en tache de fond (fenetre cachee).
rem Les 40 dernieres lignes puis suivi en direct ; Ctrl+C pour stopper.
set "STICKS3_LOG=%LOCALAPPDATA%\StickS3\service.log"
if not exist "%STICKS3_LOG%" (
    echo Aucun log pour l'instant.
    echo Le service en tache de fond ecrira ici des son premier lancement :
    echo %STICKS3_LOG%
    pause
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Get-Content -LiteralPath $env:STICKS3_LOG -Tail 40 -Wait"
echo.
pause
