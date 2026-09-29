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
    if ($Background) {
        # Fenetre cachee (tache planifiee) : sans trace ecrite, l'erreur
        # serait totalement invisible. On logge dans le fichier du service.
        $LogDir = Join-Path $env:LOCALAPPDATA 'StickS3'
        New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
        Add-Content -Path (Join-Path $LogDir 'service.log') `
            -Value ("[{0}] ERREUR lanceur : {1}" -f (Get-Date -Format 'HH:mm:ss'), $Message) `
            -Encoding Unicode
        exit 1
    }
    Read-Host 'Appuie sur Entree pour fermer' | Out-Null
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
    # Declencheur : a l'ouverture de session, REPETE toutes les 5 minutes.
    # Une instance morte (console fermee par le systeme en pleine nuit, etc.)
    # est relancee par le planificateur lui-meme en 5 min max ; s'il vit
    # deja, le demarrage est ignore (IgnoreNew).
    $Trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $Rep = New-ScheduledTaskTrigger -Once -At (Get-Date) `
        -RepetitionInterval (New-TimeSpan -Minutes 5) `
        -RepetitionDuration ([TimeSpan]::FromDays(3650))
    $Trigger.Repetition = $Rep.Repetition
    # Pas de limite de duree, batterie autorisee, redemarrage sur echec :
    # trois couches au-dessus du superviseur interne du lanceur.
    $Settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) `
        -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -RestartCount 10 -RestartInterval (New-TimeSpan -Minutes 1) `
        -StartWhenAvailable
    Register-ScheduledTask -TaskName $TaskName -Action $Action -Trigger $Trigger -Settings $Settings -Description 'Lance le service de dictee StickS3 a l ouverture de session.' -Force | Out-Null
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
    # Tache planifiee / fenetre cachee : jamais d'attente clavier ici.
    # Superviseur : une mort silencieuse du service (defaillance systeme,
    # OOM, mise a jour) est reparee dans les 5 s. Chaque redemarrage se
    # voit dans le log via la banniere "=== service StickS3 ===".
    $LogDir = Join-Path $env:LOCALAPPDATA 'StickS3'
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
    $LogFile = Join-Path $LogDir 'service.log'
    while ($true) {
        & $Python.Source $ServiceScript *>> $LogFile
        Add-Content -Path $LogFile -Value ("[{0}] superviseur : service relance apres arret" -f (Get-Date -Format 'HH:mm:ss')) -Encoding Unicode
        Start-Sleep -Seconds 5
    }
}

Write-Host 'Service StickS3 lance - laisse cette fenetre ouverte (Ctrl+C pour arreter).' -ForegroundColor Cyan
& $Python.Source $ServiceScript
$ServiceExitCode = $LASTEXITCODE
Write-Host ''
if ($ServiceExitCode -ne 0) {
    Write-Host ("Le service s'est arrete avec le code {0}. Le detail est affiche ci-dessus." -f $ServiceExitCode) -ForegroundColor Red
} else {
    Write-Host 'Service arrete.' -ForegroundColor Yellow
}
# Le .cmd appelant termine par un `pause` : la fenetre reste ouverte pour
# relire les logs, que le service se soit arrete proprement ou non.
exit $ServiceExitCode
