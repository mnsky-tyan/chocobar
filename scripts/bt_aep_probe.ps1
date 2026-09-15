# Try multiple APIs for the buds' battery
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime]
$null = [Windows.Devices.Bluetooth.BluetoothDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime]

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
  $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($Op, $T) {
  $t = $asTaskGeneric.MakeGenericMethod($T).Invoke($null, @($Op))
  $t.Wait(-1) | Out-Null
  $t.Result
}

# 1) BluetoothDevice WinRT (Win11 22H2+ battery API)
$addr = [UInt64]0xC4600A592F23  # Redmi Buds 6
try {
  $bt = Await ([Windows.Devices.Bluetooth.BluetoothDevice]::FromBluetoothAddressAsync($addr)) ([Windows.Devices.Bluetooth.BluetoothDevice])
  Write-Host ("BT device: " + $bt.Name + " conn=" + $bt.ConnectionStatus)
  $m = $bt.GetType().GetMethods() | Where-Object { $_.Name -match 'Battery' } | Select-Object -ExpandProperty Name
  Write-Host ("battery methods: " + ($m -join ', '))
  if ($m -contains 'GetBatteryPercentageAsync') {
    $r = Await ($bt.GetBatteryPercentageAsync()) ([Windows.Devices.Bluetooth.BatteryPercentage])
    Write-Host ("BATTERY: " + $r.Percentage)
  }
} catch { Write-Host "BT route failed: $($_.Exception.Message)" }

# 2) AEP store with name variants (one at a time so a bad key doesn't kill all)
foreach ($k in @('System.Devices.AepBattery_Level','System.Devices.Aep.Battery_Level','System.Devices.AepBatteryLevel')) {
  try {
    $devs = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync(
      'System.Devices.Aep:IsConnected:=System.StructuredQueryType.Boolean#True', [string[]]@('System.ItemNameDisplay',$k))) `
      ([Windows.Devices.Enumeration.DeviceInformationCollection])
    foreach ($d in $devs) {
      Write-Host ("AEP '" + $d.Properties['System.ItemNameDisplay'] + "' -> " + $k + " = " + $d.Properties[$k])
    }
  } catch { Write-Host "${k}: $($_.Exception.InnerException.Message)" }
}
