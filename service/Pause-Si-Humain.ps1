# Pause-Si-Humain.ps1 — attend une touche SEULEMENT si le script a ete
# lance par un humain (ancetre = explorer.exe, donc double-clic). Un spawn
# d'agent ou de tache planifiee referme la fenetre sans attendre personne :
# une pause sans humain = une fenetre veuve qui reste ouverte pour rien
# (bug vecu : les agents laissaient des cmd orphelines pendant des jours).
try {
    $p = Get-CimInstance Win32_Process -Filter "ProcessId=$PID"
    $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$($p.ParentProcessId)"
    $isHuman = $false
    if ($parent.Name -ieq 'cmd.exe') {
        $grand = Get-CimInstance Win32_Process -Filter "ProcessId=$($parent.ParentProcessId)"
        $isHuman = ($grand.Name -ieq 'explorer.exe')
    } else {
        $isHuman = ($parent.Name -ieq 'explorer.exe')
    }
} catch {
    $isHuman = $true  # en cas de doute : le confort humain prime
}
if ($isHuman) {
    Write-Host ''
    Read-Host 'Appuie sur Entree pour fermer cette fenetre' | Out-Null
}