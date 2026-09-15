'use strict';
// System metrics collector. Each module polls at its own cadence into a
// dirty-tracked snapshot; the main process pushes it to the bar renderer,
// at most every 250ms and only when a poll changed a value.
const os = require('os');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const native = require('./native');

// Windows implements AF_UNIX sockets on top of the named-pipe filesystem: a
// socket bound at C:\dir\file.sock is reachable as \\.\pipe\C:\dir\file.sock,
// No current module dials a socket here; the named-pipe prefix convention
// stays documented for future socket work.
const IS_WIN = process.platform === 'win32';

class MetricsEngine extends require('events') {
  constructor(config) {
    super();
    this.cfg = config;
    this._dirty = false;   // set by every poll that changed state; stats push reads-and-clears
    this.state = {
      cpu: null,          // %
      cpuTemp: null,      // { c, label } from HWiNFO shared memory, or null
      ram: null,          // { pct, usedGB, totalGB }
      gpu: null,          // { sum, max } or null
      battery: null,      // { percent, ac, charging }
      volume: null,       // { level, muted }
      bluetooth: [],      // [{ name, level }]
      volumeError: null
    };
    this._cpuPrev = os.cpus().map((c) => c.times);
    this._timers = [];    // Persistent PowerShell workers (GPU / Bluetooth), keyed by module. `gen`
    // invalidates stale exit handlers: without it a worker killed by stop()
    // respawns alongside the fresh one start() just spawned, because the
    // killed child's exit event only fires after setConfig reset _stopping.
    this._wk = { gpu: { proc: null, gen: 0 }, bt: { proc: null, gen: 0 } };
    this._respawnTimers = [];
    this._gpuBuf = '';
    this._btBuf = '';
  }

  // True at most once since the last consume — the stats push loop sends only
  // when a poll actually changed a value instead of on a fixed heartbeat.
  consumeDirty() {
    const d = this._dirty;
    this._dirty = false;
    return d;
  }

  start() {
    this._stopping = false;
    const m = this.cfg.modules;

    // Initial polls are staggered a few hundred ms apart so the cheap in-process
    // samples never align with each other or with the workers' first queries.
    if (m.cpu.enabled) {
      this._timers.push(setInterval(() => this._pollCpu(), Math.max(250, m.cpu.intervalMs)));
      setTimeout(() => { if (!this._stopping) this._pollCpu(); }, 0);
    }
    if (m.cputemp && m.cputemp.enabled) {
      this._timers.push(setInterval(() => this._pollCpuTemp(), Math.max(1000, m.cputemp.intervalMs || 2000)));
      setTimeout(() => { if (!this._stopping) this._pollCpuTemp(); }, 320);
    }
    if (m.ram.enabled) {
      this._timers.push(setInterval(() => this._pollRam(), Math.max(250, m.ram.intervalMs)));
      setTimeout(() => { if (!this._stopping) this._pollRam(); }, 80);
    }
    if (m.battery.enabled) {
      this._timers.push(setInterval(() => this._pollBattery(), Math.max(500, m.battery.intervalMs)));
      setTimeout(() => { if (!this._stopping) this._pollBattery(); }, 240);
    }
    if (m.volume.enabled && IS_WIN) {
      if (!native.initVolume(m.volume.role)) {
        this.state.volumeError = native.volumeState.error;
        console.error('[wizbar] volume init failed:', this.state.volumeError);
      }
      this._timers.push(setInterval(() => this._pollVolume(), Math.max(250, m.volume.intervalMs)));
      setTimeout(() => { if (!this._stopping) this._pollVolume(); }, 160);
    } else if (m.volume.enabled) {
      this.state.volume = null; // Core Audio is Windows-only; the chip shows "—"
    }
    // PowerShell workers cannot exist off Windows; the modules stay "—" there.
    if (m.gpu.enabled && IS_WIN) this._startGpuWorker(m.gpu);
    if (m.bluetooth.enabled && IS_WIN) this._startBtWorker(m.bluetooth);
  }

  stop() {
    this._stopping = true;
    for (const t of this._timers) clearInterval(t);
    this._timers = [];
    for (const t of this._respawnTimers) clearTimeout(t);
    this._respawnTimers = [];
    for (const k of Object.keys(this._wk)) {
      const w = this._wk[k];
      w.gen++; // any exit event from a killed worker is now stale - never respawn it
      if (w.proc) { try { w.proc.kill(); } catch (_) {} }
      w.proc = null;
    }
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
    if (total > 0) {
      const cpu = Math.round((1 - idle / total) * 100);
      if (cpu !== this.state.cpu) this._dirty = true;
      this.state.cpu = cpu;
    }
  }

  _pollCpuTemp() {
    const t = native.getHwinfoTemp();
    if (JSON.stringify(t) !== JSON.stringify(this.state.cpuTemp)) this._dirty = true;
    this.state.cpuTemp = t;
  }

  // --- RAM -------------------------------------------------------------------
  _pollRam() {
    const total = os.totalmem();
    const free = os.freemem(); // ullAvailPhys - matches Task Manager "available"
    const used = total - free;
    const ram = {
      pct: Math.round((used / total) * 100),
      usedGB: +(used / 1024 ** 3).toFixed(1),
      totalGB: +(total / 1024 ** 3).toFixed(1)
    };
    if (JSON.stringify(ram) !== JSON.stringify(this.state.ram)) this._dirty = true;
    this.state.ram = ram;
  }

  // --- battery -----------------------------------------------------------------
  _pollBattery() {
    const b = native.getBattery();
    if (JSON.stringify(b) !== JSON.stringify(this.state.battery)) this._dirty = true;
    this.state.battery = b;
  }

  // --- volume ------------------------------------------------------------------
  _pollVolume() {
    const v = native.getVolume();
    if (JSON.stringify(v) !== JSON.stringify(this.state.volume)) this._dirty = true;
    this.state.volume = v;
    if (!this.state.volume && !this.state.volumeError) {
      this.state.volumeError = native.volumeState.error;
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
    this._spawnWorker('gpu', 'powershell.exe', script, (line) => {
      try {
        const d = JSON.parse(line);
        if (d.err) { if (!this.state.gpu) this.state.gpu = { error: 'counter' }; return; }
        const gpu = {
          [mode]: Math.min(100, Math.round(d[mode])),
          sum: Math.min(100, Math.round(d.sum)),
          max: Math.min(100, Math.round(d.max))
        };
        if (JSON.stringify(gpu) !== JSON.stringify(this.state.gpu)) this._dirty = true;
        this.state.gpu = gpu;
      } catch (_) {}
    }, buf, (l) => l.startsWith('{'));
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
    this._spawnWorker('bt', 'powershell.exe', script, (line) => {
      try {
        let d = JSON.parse(line);
        if (!Array.isArray(d)) d = [d];
        const devs = d
          .filter((x) => x && typeof x.level === 'number' && x.level >= 0 && x.level <= 100)
          .filter((x) => !filter || (x.name || '').toLowerCase().includes(filter));
        devs.sort((a, b) => (a.name || '').localeCompare(b.name || ''));
        if (JSON.stringify(devs.slice(0, maxDev)) !== JSON.stringify(this.state.bluetooth)) this._dirty = true;
        this.state.bluetooth = devs.slice(0, maxDev);
      } catch (_) {}
    }, buf, (l) => l.startsWith('[') || l.startsWith('{'));
  }

  _spawnWorker(key, shell, script, onLine, buf, isLine, opts = {}) {
    const w = this._wk[key];
    const spawnIt = () => {
      if (this._stopping) return;
      const gen = ++w.gen;
      const p = spawn(shell, ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', script], {
        windowsHide: true,
        stdio: ['ignore', 'pipe', 'pipe']
      });
      w.proc = p;
      if (opts.onSpawn) opts.onSpawn();
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
      p.on('error', (e) => console.error(`[wizbar] ${key} worker error:`, e.message));
      // A dead worker must come back: without this the module shows "—" forever.
      // The generation check keeps a worker killed by stop()/setConfig from
      // resurrecting next to the fresh one start() already spawned - that race
      // used to leave two identical pollers running per config save.
      p.on('exit', (code) => {
        if (this._stopping || w.gen !== gen) return;
        if (errTail.trim()) console.error(`[wizbar] ${key} worker stderr tail:`, errTail.trim());
        // onExit returning false vetoes the respawn (e.g. permanent failure
        // that already fell back to another source).
        const veto = opts.onExit && opts.onExit() === false;
        if (veto) { console.error(`[wizbar] ${key} worker not respawned (fell back)`); return; }
        console.error(`[wizbar] ${key} worker exited (${code}); respawning in 2s`);
        this._respawnTimers.push(setTimeout(() => {
          if (!this._stopping && w.gen === gen) spawnIt();
        }, 2000));
      });
    };
    spawnIt();
  }

  snapshot() {
    return { ...this.state };
  }
}

module.exports = { MetricsEngine };
