$devs = Get-PnpDevice -Status OK -ErrorAction SilentlyContinue
foreach ($d in $devs) {
  $p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName '{83DA6326-97A6-4088-9453-A1923F573B29} 2' -ErrorAction SilentlyContinue
  if ($null -ne $p -and $null -ne $p.Data) {
    Write-Host ("BATTERY: " + $d.FriendlyName + " => " + $p.Data)
  }
}
Write-Host "DONE"
