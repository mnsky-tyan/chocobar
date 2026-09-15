# Full registry dump for the Redmi Buds BT cache
function Dump($path, $depth) {
  if ($depth -gt 4) { return }
  $p = Get-ItemProperty $path -ErrorAction SilentlyContinue
  if ($p) {
    foreach ($prop in $p.PSObject.Properties) {
      if ($prop.Name -match '^PS') { continue }
      $v = $prop.Value
      if ($v -is [byte[]]) {
        $hex = ($v | Select-Object -First 24 | ForEach-Object { $_.ToString('X2') }) -join ' '
        $ascii = (($v | Select-Object -First 40) | ForEach-Object { if ($_ -ge 32 -and $_ -le 126) { [char]$_ } else { '.' } }) -join ''
        Write-Host ('  ' * $depth + $prop.Name + " = [" + $hex + "]  '" + $ascii + "'")
      } else {
        Write-Host ('  ' * $depth + $prop.Name + " = " + $v)
      }
    }
  }
  Get-ChildItem $path -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host ('  ' * $depth + '[' + $_.PSChildName + ']')
    Dump $_.PSPath ($depth + 1)
  }
}
Dump 'HKLM:\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices\c4600a592f23' 0
