param(
    [switch]$Background,
    [switch]$InstallStartup
)

$ErrorActionPreference = 'Stop'
$ServiceDir = $PSScriptRoot
$ServiceScript = Join-Path $ServiceDir 'sticks3_service.py'
$Requirements = Join-Path $ServiceDir 'requirements.txt'
$TaskName = 'StickS3 Service'

function Stop-WithMessage([string]$Message) {
    Write-Host "`nERREUR - $Message" -ForegroundColor Red
    if (-not $Background) { Read-Host 'Appuie sur Entree pour fermer' | Out-Null }
    exit 1
}

$Python = Get-Command py -ErrorAction SilentlyContinue
if (-not $Python) {
    Stop-WithMessage "Python n'est pas installe. Installe Python 3 depuis https://www.python.org/downloads/windows/ puis relance ce fichier. Coche 'Add python.exe to PATH' pendant l'installation."
}

try {
    & $Python.Source -c 'import requests' 2>$null
    if ($LASTEXITCODE -ne 0) { throw 'requests absent' }
} catch {
    if ($Background) { Stop-WithMessage "La dependance Python 'requests' manque. Lance d'abord Lancer-StickS3.cmd une fois." }
    Write-Host "Installation de la dependance Python manquante..." -ForegroundColor Yellow
    & $Python.Source -m pip install -r $Requirements
    if ($LASTEXITCODE -ne 0) { Stop-WithMessage "Impossible d'installer les dependances Python." }
}

# La cle vit dans les variables d'environnement du compte Windows, pas dans le
# depot. On accepte aussi une cle OpenAI existante sans la dupliquer.
$ApiKey = $env:STT_API_KEY
if (-not $ApiKey) { $ApiKey = [Environment]::GetEnvironmentVariable('STT_API_KEY', 'User') }
if (-not $ApiKey) { $ApiKey = $env:OPENAI_API_KEY }
if (-not $ApiKey) { $ApiKey = [Environment]::GetEnvironmentVariable('OPENAI_API_KEY', 'User') }

if (-not $ApiKey) {
    if ($Background) { Stop-WithMessage "Cle API absente. Lance d'abord Lancer-StickS3.cmd une fois pour l'enregistrer." }
    $SecureKey = Read-Host 'Cle API de transcription (elle ne sera demandee qu une fois)' -AsSecureString
    $ApiKey = [System.Net.NetworkCredential]::new('', $SecureKey).Password
    if (-not $ApiKey) { Stop-WithMessage 'Aucune cle saisie.' }
    [Environment]::SetEnvironmentVariable('STT_API_KEY', $ApiKey, 'User')
    Write-Host 'Cle enregistree pour ce compte Windows.' -ForegroundColor Green
}
$env:STT_API_KEY = $ApiKey

if ($InstallStartup) {
    $PowerShell = (Get-Command powershell.exe).Source
    $Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`" -Background"
    $Action = New-ScheduledTaskAction -Execute $PowerShell -Argument $Arguments
    $Trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    Register-ScheduledTask -TaskName $TaskName -Action $Action -Trigger $Trigger -Description 'Lance le service de dictee StickS3 a l ouverture de session.' -Force | Out-Null
    Write-Host 'Demarrage automatique ajoute pour ce compte Windows.' -ForegroundColor Green
}

# Un second lancement ne cree pas une seconde instance qui se battrait pour les ports.
try {
    $Healthy = Invoke-WebRequest -UseBasicParsing -Uri 'http://127.0.0.1:8788/health' -TimeoutSec 1
    if ($Healthy.StatusCode -eq 200) {
        $Health = $Healthy.Content | ConvertFrom-Json
        if ($Health.discovery -eq $true) {
            if (-not $Background) { Write-Host 'Le service StickS3 est deja lance.' -ForegroundColor Yellow }
            exit 0
        }
        Stop-WithMessage "Une ancienne version du service StickS3 utilise deja le port. Arrete-la avec Ctrl+C dans son terminal, puis relance ce fichier."
    }
} catch { }

if ($Background) {
    $LogDir = Join-Path $env:LOCALAPPDATA 'StickS3'
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
    & $Python.Source $ServiceScript *>> (Join-Path $LogDir 'service.log')
} else {
    Write-Host 'Service StickS3 lance - laisse cette fenetre ouverte.' -ForegroundColor Cyan
    & $Python.Source $ServiceScript
}

# Un programme Python qui s'arrete en erreur ne fait pas automatiquement
# echouer PowerShell. Propager son code garde la fenetre .cmd ouverte et rend
# le diagnostic lisible au lieu de la fermer silencieusement.
$ServiceExitCode = $LASTEXITCODE
if ($ServiceExitCode -ne 0) {
    Stop-WithMessage "Le service Python s'est arrete avec le code $ServiceExitCode. Le detail est affiche ci-dessus."
}
exit 0
