# CPU profile of the wizbar electron tree + its PowerShell pollers over N seconds.
param([int]$Seconds = 45)
$p0 = @{}
Get-Process electron, powershell -ErrorAction SilentlyContinue | ForEach-Object { $p0["$($_.Id)"] = $_.CPU }
Start-Sleep -Seconds $Seconds
$roles = @{}
Get-CimInstance Win32_Process | Where-Object { $_.Name -in @('electron.exe', 'powershell.exe') } | ForEach-Object {
  $cl = $_.CommandLine
  if ($cl -match 'GPU Engine') { $roles["$($_.ProcessId)"] = 'ps-gpu-poller' }
  elseif ($cl -match 'Get-PnpDevice') { $roles["$($_.ProcessId)"] = 'ps-bt-poller' }
  elseif ($cl -match 'Get-CimInstance') { $roles["$($_.ProcessId)"] = 'ps-query(self)' }
  elseif ($_.Name -eq 'powershell.exe') { $roles["$($_.ProcessId)"] = 'ps-other' }
  elseif ($cl -match 'type=renderer') { $roles["$($_.ProcessId)"] = 'renderer' }
  elseif ($cl -match 'type=gpu-process') { $roles["$($_.ProcessId)"] = 'gpu-process' }
  elseif ($cl -match 'type=utility') { $roles["$($_.ProcessId)"] = 'utility' }
  else { $roles["$($_.ProcessId)"] = 'MAIN' }
}
$total = 0
Get-Process electron, powershell -ErrorAction SilentlyContinue | ForEach-Object {
  $d = $_.CPU - $p0["$($_.Id)"]
  $pct = [math]::Round($d / $Seconds * 100, 1)
  if ($pct -lt 0.05) { return }
  $total += $pct
  '{0,-14} pid={1,-7} cpu={2}%' -f $roles["$($_.Id)"], $_.Id, $pct
}
'TOTAL cpu = ' + $total + '% of a core'
