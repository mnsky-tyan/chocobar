# Try to read the standard GATT Battery Service (0x180F) from the buds' LE side
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime]
$null = [Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceService, Windows.Devices.Bluetooth, ContentType=WindowsRuntime]
$null = [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType=WindowsRuntime]

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
  $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($Op, $T) {
  $t = $asTaskGeneric.MakeGenericMethod($T).Invoke($null, @($Op))
  $t.Wait(-1) | Out-Null
  $t.Result
}

# 1) find bonded/known Bluetooth LE devices
$sel = 'System.Devices.AepProtocolId:="{C1DF4A1F-3E63-4FCD-A3D3-67CDBB2B2C2F}"'
try {
  $devs = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync(
    $sel, [string[]]@('System.ItemNameDisplay'))) ([Windows.Devices.Enumeration.DeviceInformationCollection])
  Write-Host ("BLE AEPs: " + $devs.Count)
  foreach ($d in $devs) { Write-Host ("  BLE: '" + $d.Properties['System.ItemNameDisplay'] + "' id=" + $d.Id) }
} catch { Write-Host "BLE enum failed: $($_.Exception.InnerException.Message)" }

# 2) direct: LE side of the buds uses the paired LE address 48A659D1CD0D
try {
  $le = Await ([Windows.Devices.Bluetooth.BluetoothLEDevice]::FromBluetoothAddressAsync([UInt64]0x48A659D1CD0D)) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
  if ($null -eq $le) { Write-Host 'LE device not available (address may have rotated)'; exit }
  Write-Host ("LE device: " + $le.Name + " conn=" + $le.ConnectionStatus)
  $res = Await ($le.GetGattServicesForUuidAsync([guid]'0000180f-0000-1000-8000-00805f9b34fb')) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattServicesResult])
  Write-Host ("battery service status: " + $res.Status + " count=" + $res.Services.Count)
  if ($res.Services.Count -gt 0) {
    $svc = $res.Services[0]
    $ch = Await ($svc.GetCharacteristicsForUuidAsync([guid]'00002a19-0000-1000-8000-00805f9b34fb')) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristicsResult])
    if ($ch.Status -eq 'Success' -and $ch.Characteristics.Count -gt 0) {
      $v = Await ($ch.Characteristics[0].ReadValueAsync()) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattReadResult])
      $bytes = [System.Linq.Enumerable]::ToArray([System.IO.StreamReader]::new($v.Value.AsStream()).BaseStream) 2>$null
      $dr = [Windows.Storage.Streams.DataReader]::FromBuffer($v.Value)
      $arr = New-Object byte[] $dr.UnconsumedBufferLength
      $dr.ReadBytes($arr) | Out-Null
      Write-Host ("BATTERY LEVEL: " + $arr[0] + "%")
    } else { Write-Host ("char status: " + $ch.Status) }
  }
} catch { Write-Host "GATT route failed: $($_.Exception.Message)" }
