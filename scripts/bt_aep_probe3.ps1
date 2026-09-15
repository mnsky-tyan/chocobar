$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime]

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
  $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($Op, $T, $Ms = 4000) {
  $t = $asTaskGeneric.MakeGenericMethod($T).Invoke($null, @($Op))
  if (-not $t.Wait($Ms)) { throw 'timeout' }
  return $t.Result
}

# ALL devices (empty selector), filter by name
$devs = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync(
  '',
  [string[]]@('System.ItemNameDisplay'))) ([Windows.Devices.Enumeration.DeviceInformationCollection]) 15000
Write-Host ("AEPs total: " + $devs.Count)
foreach ($d in $devs) {
  $n = $d.Properties['System.ItemNameDisplay']
  if ($n -and ($n -match 'Redmi|Buds')) {
    Write-Host ("FOUND: '" + $n + "' id=" + $d.Id)
    foreach ($k in @('System.Devices.AepBattery_Level', '{0833E2A2-D443-4E3D-95A0-22DC33C44B32} 12')) {
      try {
        $di = Await ([Windows.Devices.Enumeration.DeviceInformation]::CreateFromIdAsync($d.Id, [string[]]@($k))) ([Windows.Devices.Enumeration.DeviceInformation]) 4000
        $v = $di.Properties[$k]
        Write-Host ("   " + $k + " = " + $(if ($v) { $v } else { '(empty)' }))
      } catch { Write-Host ("   " + $k + " -> " + $_.Exception.InnerException.Message) }
    }
  }
}
Write-Host 'done'
