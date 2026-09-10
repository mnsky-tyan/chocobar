'use strict';
// Reproduce the exact GPU worker script from metrics.js and run it briefly.
const { spawn } = require('child_process');
const script = [
  'while($true){',
  '  try{',
  "    $s=(Get-Counter '\\GPU Engine(*)\\Utilization Percentage' -ErrorAction Stop).CounterSamples | Where-Object {$_.CookedValue -gt 0}",
  '    if($s){',
  '      $sum=[math]::Round(($s|Measure-Object CookedValue -Sum).Sum,1)',
  '      $mx=[math]::Round(($s|Measure-Object CookedValue -Maximum).Maximum,1)',
  '      (\'{"sum":\' + $sum + \',"max":\' + $mx + \'}\')',
  "    } else { '{\"sum\":0,\"max\":0}' }",
  '  }catch{ \'{"err":1}\'; Write-Error $_ }',
  '  Start-Sleep -Milliseconds 500',
  '}'
].join('\n');
require('fs').writeFileSync(__dirname + '\\gpu_worker_debug.ps1', script);
const p = spawn('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', script], {
  windowsHide: true, stdio: ['ignore', 'pipe', 'pipe']
});
let out = '', err = '';
p.stdout.setEncoding('utf8');
p.stdout.on('data', (c) => { out += c; });
p.stderr.setEncoding('utf8');
p.stderr.on('data', (c) => { err += c; });
p.on('exit', (code) => {
  console.log('EXIT', code);
  console.log('STDOUT:', out.trim().slice(0, 500) || '(none)');
  console.log('STDERR:', err.trim().slice(0, 800) || '(none)');
});
setTimeout(() => { try { p.kill(); } catch (_) {} }, 9000);
