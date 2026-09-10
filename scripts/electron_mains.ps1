# Show electron main processes (no --type= flag = main)
Get-CimInstance Win32_Process -Filter "Name='electron.exe'" | ForEach-Object {
  $c = $_.CommandLine
  $isMain = ($c -notmatch '--type=')
  "pid=$($_.ProcessId) main=$isMain parent=$($_.ParentProcessId)"
}
