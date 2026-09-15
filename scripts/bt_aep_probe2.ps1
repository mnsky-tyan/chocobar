# Per-device battery probe with hard timeouts (can't hang)
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

try {
  $devs = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync(
    'System.Devices.Aep:IsConnected:=System.StructuredQueryType.Boolean#True',
    [string[]]@('System.ItemNameDisplay'))) ([Windows.Devices.Enumeration.DeviceInformationCollection]) 6000
  Write-Host ("connected AEPs: " + $devs.Count)
  foreach ($d in $devs) {
    Write-Host ("-- '" + $d.Properties['System.ItemNameDisplay'] + "'")
    foreach ($k in @('System.Devices.AepBattery_Level', '{0833E2A2-D443-4E3D-95A0-22DC33C44B32} 12')) {
      try {
        $di = Await ([Windows.Devices.Enumeration.DeviceInformation]::CreateFromIdAsync($d.Id, [string[]]@($k))) ([Windows.Devices.Enumeration.DeviceInformation]) 4000
        $v = $di.Properties[$k]
        if ($v) { Write-Host ("   " + $k + " = " + $v) } else { Write-Host ("   " + $k + " = (empty)") }
      } catch { Write-Host ("   " + $k + " -> " + $_.Exception.Message) }
    }
  }
} catch { Write-Host ("enum failed: " + $_.Exception.Message) }

Write-Host '--- PnP battery property (buds connected now) ---'
Get-PnpDevice -Status OK -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'BTHENUM|BTHLE' } | ForEach-Object {
  $p = $_ | Get-PnpDeviceProperty -KeyName '{83DA6326-97A6-4088-9453-A1923F573B29} 2' -ErrorAction SilentlyContinue
  if ($null -ne $p -and $null -ne $p.Data) { Write-Host ("PnP BATTERY: " + $_.FriendlyName + " = " + $p.Data) }
}
Write-Host 'done'
