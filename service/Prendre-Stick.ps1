# ------------------------------------------------------------
# Prendre-Stick.ps1 - appelle le Stick vers CE PC.
# Envoie la reclamation UDP depuis chaque interface reelle, vers le
# broadcast de sous-reseau ET le broadcast limite, plusieurs fois :
# les adaptateurs virtuels et certains boxes filtrent selon le cas.
# ------------------------------------------------------------
$ErrorActionPreference = 'Stop'

$TcpPort = 8787
$CfgPath = Join-Path $PSScriptRoot 'config.json'
if (Test-Path -LiteralPath $CfgPath) {
    try {
        $Cfg = Get-Content -LiteralPath $CfgPath -Raw | ConvertFrom-Json
        if ($Cfg.listen_port) { $TcpPort = [int]$Cfg.listen_port }
    } catch { }
}

# Nom du PC inclus dans la reclamation : le Stick sait a qui il parle.
$PcName = [regex]::Replace($env:COMPUTERNAME, '[^A-Za-z0-9-]', '')
if ($PcName.Length -gt 16) { $PcName = $PcName.Substring(0, 16) }
$Payload = [Text.Encoding]::ASCII.GetBytes(('STICKS3_CLAIM_V1 {0} {1}' -f $TcpPort, $PcName))

$Targets = [System.Collections.Generic.List[string]]::new()
$Targets.Add('255.255.255.255')

$SentFrom = [System.Collections.Generic.List[string]]::new()
$Ifaces = [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() |
    Where-Object { $_.OperationalStatus -eq 'Up' -and $_.NetworkInterfaceType -ne 'Loopback' }

foreach ($If in $Ifaces) {
    $Unicast = $If.GetIPProperties().UnicastAddresses |
        Where-Object { $_.Address.AddressFamily -eq 'InterNetwork' -and $_.IPv4Mask }
    foreach ($Ua in $Unicast) {
        $IpBytes = $Ua.Address.GetAddressBytes()
        $MaskBytes = $Ua.IPv4Mask.GetAddressBytes()
        $BcBytes = [byte[]]::new(4)
        for ($i = 0; $i -lt 4; $i++) {
            $BcBytes[$i] = $IpBytes[$i] -bor (255 -bxor $MaskBytes[$i])
        }
        $Broadcast = [System.Net.IPAddress]::new($BcBytes).ToString()
        if ($Targets -notcontains $Broadcast) { $Targets.Add($Broadcast) }

        # socket lie a CETTE interface : le paquet part forcement d'ici
        try {
            $Local = [System.Net.IPEndPoint]::new($Ua.Address, 0)
            $Udp = [System.Net.Sockets.UdpClient]::new($Local)
            $Udp.EnableBroadcast = $true
            for ($Try = 0; $Try -lt 3; $Try++) {
                foreach ($T in $Targets) {
                    [void]$Udp.Send($Payload, $Payload.Length, $T, 8789)
                }
                Start-Sleep -Milliseconds 150
            }
            $Udp.Dispose()
            $SentFrom.Add($Ua.Address.ToString())
        } catch { }
    }
}

if ($SentFrom.Count -eq 0) {
    Write-Host 'Aucune interface reseau disponible pour appeler le Stick.' -ForegroundColor Red
    exit 1
}
Write-Host ('Stick appele depuis {0} - il bascule sur ce PC en quelques secondes.' -f ($SentFrom -join ', '))
