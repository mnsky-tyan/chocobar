# Probe every plausible source for Bluetooth earbud battery
Write-Host '=== Win32_Battery (all) ==='
Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Format-List Name, EstimatedChargeRemaining, BatteryStatus, DeviceID

Write-Host '=== PnP class Battery ==='
Get-PnpDevice -Class Battery -ErrorAction SilentlyContinue | Format-List FriendlyName, InstanceId, Status, Problem

Write-Host '=== All Redmi/Buds PnP entities ==='
$devs = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -match 'Redmi|Buds|Redm' -or $_.InstanceId -match 'C4600A592F23' }
$devs | ForEach-Object {
  "DEV: $($_.FriendlyName) | $($_.InstanceId) | status=$($_.Status)"
  foreach ($k in @('{83DA6326-97A6-4088-9453-A1923F573B29} 2', '{83DA6326-97A6-4088-9453-A1923F573B29} 1', '{83DA6326-97A6-4088-9453-A1923F573B29} 3')) {
    try {
      $v = ($_ | Get-PnpDeviceProperty -KeyName $k -ErrorAction Stop).Data
      if ($null -ne $v) { "   prop ${k}: $v" }
    } catch {}
  }
}

Write-Host '=== BTHPORT registry devices ==='
Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices' -ErrorAction SilentlyContinue | ForEach-Object {
  $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
  if ($p.Name) { $n = ($p.Name | ForEach-Object { [char]$_ }) -join '' } else { $n = '?' }
  "$($_.PSChildName) -> $n"
}
