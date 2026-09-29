' Lancer-Fond.vbs — lance le superviseur StickS3 sans AUCUNE console :
' wscript n'a pas de console, donc pas de flash de fenêtre à chaque
' passage du chien de garde du planificateur.
Set sh = CreateObject("WScript.Shell")
cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File """ & _
      Replace(WScript.ScriptFullName, "Lancer-Fond.vbs", "Start-StickS3.ps1") & _
      """ -Background"
sh.Run cmd, 0, False