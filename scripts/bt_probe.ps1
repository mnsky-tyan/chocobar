Write-Host "=== Bluetooth devices (status OK) ==="
Get-PnpDevice -Class Bluetooth -Status OK -ErrorAction SilentlyContinue |
  Select-Object FriendlyName, InstanceId | Format-List

Write-Host "=== ALL devices reporting a battery property ==="
$devs = Get-PnpDevice -Status OK -ErrorAction SilentlyContinue
foreach ($d in $devs) {
  $p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName '{83DA6326-97A6-4088-9453-A1923F573B29} 2' -ErrorAction SilentlyContinue
  if ($null -ne $p -and $null -ne $p.Data) {
    Write-Host ("BATTERY: " + $d.FriendlyName + " => " + $p.Data)
  }
}

Write-Host "=== Properties of first BT device (hunt for mode/ANC) ==="
$bt = Get-PnpDevice -Class Bluetooth -Status OK -ErrorAction SilentlyContinue | Select-Object -First 1
if ($bt) {
  Write-Host ("Device: " + $bt.FriendlyName)
  Get-PnpDeviceProperty -InstanceId $bt.InstanceId -ErrorAction SilentlyContinue |
    Where-Object { $_.Data -ne $null } |
    ForEach-Object { Write-Host ("  " + $_.KeyName + " = " + $_.Data) }
}
