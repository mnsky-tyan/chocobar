while($true){
  try{
    $s=(Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction Stop).CounterSamples | Where-Object {$_.CookedValue -gt 0}
    if($s){
      $sum=[math]::Round(($s|Measure-Object CookedValue -Sum).Sum,1)
      $mx=[math]::Round(($s|Measure-Object CookedValue -Maximum).Maximum,1)
      ('{"sum":' + $sum + ',"max":' + $mx + '}')
    } else { '{"sum":0,"max":0}' }
  }catch{ '{"err":1}'; Write-Error $_ }
  Start-Sleep -Milliseconds 500
}