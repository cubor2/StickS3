@echo off
setlocal
rem Appelle le Stick vers CE PC : broadcast UDP entendu par le Stick,
rem meme si son service tourne encore sur une autre machine.
set "STICKS3_CFG=%~dp0config.json"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ^
  "$p=8787; if(Test-Path -LiteralPath $env:STICKS3_CFG){ try{ $j=Get-Content -LiteralPath $env:STICKS3_CFG -Raw | ConvertFrom-Json; if($j.listen_port){ $p=[int]$j.listen_port } }catch{} }; $u=[System.Net.Sockets.UdpClient]::new(); $u.EnableBroadcast=$true; $b=[Text.Encoding]::ASCII.GetBytes(('STICKS3_CLAIM_V1 {0}' -f $p)); [void]$u.Send($b,$b.Length,'255.255.255.255',8789); $u.Dispose(); Write-Host 'Stick appele : il bascule sur ce PC en quelques secondes.'"
if errorlevel 1 pause
