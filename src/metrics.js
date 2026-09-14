'use strict';
// System metrics collector. Each module polls at its own cadence; a 1s ticker
// emits a combined snapshot for the bar renderer.
const os = require('os');
const fs = require('fs');
const { spawn } = require('child_process');
const native = require('./native');

class MetricsEngine extends require('events') {
  constructor(config) {
    super();
    this.cfg = config;
    this.state = {
      cpu: null,          // %
      cpuTemp: null,      // { c, label } from HWiNFO shared memory, or null
      ram: null,          // { pct, usedGB, totalGB }
      gpu: null,          // { sum, max } or null
      battery: null,      // { percent, ac, charging }
      volume: null,       // { level, muted }
      bluetooth: [],      // [{ name, level }]
      agents: null,       // { state: working|idle|unknown, count, note } from the fleet status file
      volumeError: null
    };
    this._cpuPrev = os.cpus().map((c) => c.times);
    this._timers = [];
    this._gpuProc = null;
    this._btProc = null;
    this._gpuBuf = '';
    this._btBuf = '';
  }

  start() {
    this._stopping = false;
    const m = this.cfg.modules;

    if (m.cpu.enabled) {
      this._timers.push(setInterval(() => this._pollCpu(), Math.max(250, m.cpu.intervalMs)));
      this._pollCpu();
    }
    if (m.cputemp && m.cputemp.enabled) {
      this._timers.push(setInterval(() => this._pollCpuTemp(), Math.max(1000, m.cputemp.intervalMs || 2000)));
      this._pollCpuTemp();
    }
    if (m.ram.enabled) {
      this._timers.push(setInterval(() => this._pollRam(), Math.max(250, m.ram.intervalMs)));
      this._pollRam();
    }
    if (m.battery.enabled) {
      this._timers.push(setInterval(() => this._pollBattery(), Math.max(500, m.battery.intervalMs)));
      this._pollBattery();
    }
    if (m.volume.enabled) {
      if (!native.initVolume(m.volume.role)) {
        this.state.volumeError = native.volumeState.error;
        console.error('[wizbar] volume init failed:', this.state.volumeError);
      }
      this._timers.push(setInterval(() => this._pollVolume(), Math.max(250, m.volume.intervalMs)));
      this._pollVolume();
    }
    if (m.gpu.enabled) this._startGpuWorker(m.gpu);
    if (m.bluetooth.enabled) this._startBtWorker(m.bluetooth);
    if (m.agents && m.agents.enabled) {
      this._timers.push(setInterval(() => this._pollAgents(), Math.max(2000, m.agents.intervalMs || 5000)));
      this._pollAgents();
    }
  }

  stop() {
    this._stopping = true;
    for (const t of this._timers) clearInterval(t);
    this._timers = [];
    for (const p of [this._gpuProc, this._btProc]) {
      if (p) { try { p.kill(); } catch (_) {} }
    }
    this._gpuProc = this._btProc = null;
  }

  setConfig(config) {
    // Restart the polling engine only when module settings actually changed —
    // every restart orphans the PowerShell workers (GPU shows "—" for seconds
    // during their cold start), and config saves can be pure no-ops.
    const sig = JSON.stringify([config.modules, config.terminal]);
    this.cfg = config;
    if (sig === this._sig) return;
    this._sig = sig;
    this.stop();
    this._stopping = false;
    this.start();
  }

  // --- CPU -------------------------------------------------------------------
  _pollCpu() {
    const next = os.cpus().map((c) => c.times);
    let idle = 0, total = 0;
    for (let i = 0; i < next.length; i++) {
      const a = this._cpuPrev[i] || next[i], b = next[i];
      const didle = b.idle - a.idle;
      const dtotal = (b.user - a.user) + (b.nice - a.nice) + (b.sys - a.sys) + (b.irq - a.irq);
      idle += didle; total += dtotal + didle;
    }
    this._cpuPrev = next;
    if (total > 0) this.state.cpu = Math.round((1 - idle / total) * 100);
  }

  _pollCpuTemp() {
    this.state.cpuTemp = native.getHwinfoTemp();
  }

  // --- RAM -------------------------------------------------------------------
  _pollRam() {
    const total = os.totalmem();
    const free = os.freemem(); // ullAvailPhys - matches Task Manager "available"
    const used = total - free;
    this.state.ram = {
      pct: Math.round((used / total) * 100),
      usedGB: +(used / 1024 ** 3).toFixed(1),
      totalGB: +(total / 1024 ** 3).toFixed(1)
    };
  }

  // --- battery -----------------------------------------------------------------
  _pollBattery() {
    this.state.battery = native.getBattery();
  }

  // --- volume ------------------------------------------------------------------
  _pollVolume() {
    this.state.volume = native.getVolume();
    if (!this.state.volume && !this.state.volumeError) {
      this.state.volumeError = native.volumeState.error;
    }
  }

  // --- agent fleet activity -----------------------------------------------------
  // The main firstmate keeps a tiny status file updated; JSON preferred,
  // {"state":"working"|"idle","agents":N,"note":"..."}, but a bare first line
  // containing "working"/"idle" is accepted too. Missing file = null (dim dash).
  _pollAgents() {
    const cfgFile = this.cfg.modules.agents && this.cfg.modules.agents.file;
    if (!cfgFile) { this.state.agents = null; return; }
    try {
      const raw = fs.readFileSync(cfgFile.replace(/^~/, os.homedir()), 'utf8');
      let d = null;
      try { d = JSON.parse(raw); } catch (_) {}
      let out;
      if (d && typeof d === 'object') {
        out = { state: String(d.state || 'unknown').toLowerCase(), count: d.agents, note: d.note };
      } else {
        out = { state: raw.split(/\r?\n/)[0].trim().toLowerCase() || 'unknown' };
      }
      if (out.state !== 'working' && out.state !== 'idle') {
        out.state = /work|busy|active/.test(out.state) ? 'working' : /idle|free/.test(out.state) ? 'idle' : 'unknown';
      }
      this.state.agents = out;
    } catch (_) {
      this.state.agents = null; // no file yet
    }
  }

  // --- GPU (PowerShell GPU Engine perf counters, persistent worker) -------------
  _startGpuWorker(cfg) {
    const mode = cfg.mode === 'max' ? 'max' : 'sum';
    const script = [
      'while($true){',
      '  try{',
      "    $s=(Get-Counter '\\GPU Engine(*)\\Utilization Percentage' -ErrorAction Stop).CounterSamples | Where-Object {$_.CookedValue -gt 0}",
      '    if($s){',
      '      $sum=[math]::Round(($s|Measure-Object CookedValue -Sum).Sum,1)',
      '      $mx=[math]::Round(($s|Measure-Object CookedValue -Maximum).Maximum,1)',
      // PowerShell has no \" escape in double-quoted strings — build the JSON
      // with single-quoted literals or the script dies with a parse error.
      '      (\'{"sum":\' + $sum + \',"max":\' + $mx + \'}\')',
      "    } else { '{\"sum\":0,\"max\":0}' }",
      '  }catch{ \'{"err":1}\'; Write-Error $_ }',
      `  Start-Sleep -Milliseconds ${Math.max(5000, Math.min(cfg.intervalMs, 8000))}`,
      '}'
    ].join('\n');
    const buf = { v: '' };
    this._spawnWorker(script, (line) => {
      try {
        const d = JSON.parse(line);
        if (d.err) { if (!this.state.gpu) this.state.gpu = { error: 'counter' }; return; }
        this.state.gpu = {
          [mode]: Math.min(100, Math.round(d[mode])),
          sum: Math.min(100, Math.round(d.sum)),
          max: Math.min(100, Math.round(d.max))
        };
      } catch (_) {}
    }, (p) => { this._gpuProc = p; }, buf, (l) => l.startsWith('{'));
  }

  // --- Bluetooth battery (PnP property {83DA6326...},2) --------------------------
  _startBtWorker(cfg) {
    const filter = (cfg.filter || '').toLowerCase();
    const maxDev = Math.max(1, cfg.maxDevices || 2);
    const script = [
      'while($true){',
      '  $out=@()',
      '  Get-PnpDevice -Class Bluetooth -Status OK -ErrorAction SilentlyContinue | ForEach-Object {',
      "    $p=Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName '{83DA6326-97A6-4088-9453-A1923F573B29} 2' -ErrorAction SilentlyContinue",
      '    if($null -ne $p.Data){',
      '      $out += [pscustomobject]@{name=$_.FriendlyName; level=[int]$p.Data}',
      '    }',
      '  }',
      "  if($out.Count -gt 0){ $out | ConvertTo-Json -Compress } else { '[]' }",
      `  Start-Sleep -Seconds ${Math.max(10, Math.round(cfg.intervalMs / 1000))}`,
      '}'
    ].join('\n');
    const buf = { v: '' };
    this._spawnWorker(script, (line) => {
      try {
        let d = JSON.parse(line);
        if (!Array.isArray(d)) d = [d];
        const devs = d
          .filter((x) => x && typeof x.level === 'number' && x.level >= 0 && x.level <= 100)
          .filter((x) => !filter || (x.name || '').toLowerCase().includes(filter));
        devs.sort((a, b) => (a.name || '').localeCompare(b.name || ''));
        this.state.bluetooth = devs.slice(0, maxDev);
      } catch (_) {}
    }, (p) => { this._btProc = p; }, buf, (l) => l.startsWith('[') || l.startsWith('{'));
  }

  _spawnWorker(script, onLine, onSpawn, buf, isLine) {
    const spawnIt = () => {
      const p = spawn('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', script], {
        windowsHide: true,
        stdio: ['ignore', 'pipe', 'pipe']
      });
      onSpawn(p);
      p.stdout.setEncoding('utf8');
      p.stdout.on('data', (chunk) => {
        buf.v += chunk;
        const parts = buf.v.split(/\r?\n/);
        buf.v = parts.pop() || '';
        for (const l of parts) {
          const t = l.trim();
          if (t && isLine(t)) onLine(t);
        }
      });
      let errTail = '';
      p.stderr.setEncoding('utf8');
      p.stderr.on('data', (c) => { errTail = (errTail + c).slice(-400); });
      p.on('error', (e) => console.error('[wizbar] worker error:', e.message));
      // A dead worker must come back: without this the module shows "—" forever.
      p.on('exit', (code) => {
        if (this._stopping) return;
        if (errTail.trim()) console.error('[wizbar] worker stderr tail:', errTail.trim());
        console.error(`[wizbar] worker exited (${code}); respawning in 2s`);
        setTimeout(() => { if (!this._stopping) spawnIt(); }, 2000);
      });
    };
    spawnIt();
  }

  snapshot() {
    return { ...this.state, now: new Date().toISOString() };
  }
}

module.exports = { MetricsEngine };
