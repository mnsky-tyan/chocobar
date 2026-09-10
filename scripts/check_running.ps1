# Diagnostic: identify electron processes + terminal windows
Get-CimInstance Win32_Process -Filter "Name='electron.exe'" | ForEach-Object {
  $c = $_.CommandLine
  if ($c -and $c.Length -gt 200) { $c = $c.Substring(0, 200) }
  "PID $($_.ProcessId): $c"
}
"--- windows-terminal ---"
Get-Process -Name WindowsTerminal -ErrorAction SilentlyContinue | ForEach-Object {
  "PID $($_.Id) title='$($_.MainWindowTitle)'"
}
"--- wizbar window ---"
Get-Process -Name electron -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle } | ForEach-Object {
  "PID $($_.Id) title='$($_.MainWindowTitle)'"
}
