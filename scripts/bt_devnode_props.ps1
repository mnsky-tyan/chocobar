# Dump ALL properties of the Redmi Buds devnodes; print battery-ish ones
$ids = @(
  'BTHENUM\DEV_C4600A592F23\7&30F358C1&0&BLUETOOTHDEVICE_C4600A592F23',
  'BTHENUM\{0000110C-0000-1000-8000-00805F9B34FB}_VID&000102B0_PID&0000\7&30F358C1&0&C4600A592F23_C00000000',
  'BTHENUM\{0000110E-0000-1000-8000-00805F9B34FB}_VID&000102B0_PID&0000\7&30F358C1&0&C4600A592F23_C00000000',
  'BTHENUM\{0000FD2D-0000-1000-8000-00805F9B34FB}_VID&000102B0_PID&0000\7&30F358C1&0&C4600A592F23_C00000000'
)
foreach ($id in $ids) {
  Write-Host "=== $id"
  try {
    Get-PnpDeviceProperty -InstanceId $id -ErrorAction Stop | ForEach-Object {
      $kn = $_.KeyName; $v = $_.Data
      if ($kn -match 'Battery|Power|Charge' -or ($v -is [int] -and $v -ge 0 -and $v -le 100 -and $kn -match 'Percent|Level')) {
        Write-Host ("  " + $kn + " = " + $v)
      }
    }
  } catch { Write-Host "  (no devnode: $($_.Exception.Message))" }
}
