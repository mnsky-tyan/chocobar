'use strict';
// System metrics collector. Each module polls at its own cadence; a 1s ticker
// emits a combined snapshot for the bar renderer.
const os = require('os');
const fs = require('fs');
const net = require('net');
const path = require('path');
const { spawn } = require('child_process');
const native = require('./native');

// Windows implements AF_UNIX sockets on top of the named-pipe filesystem: a
// socket bound at C:\dir\file.sock is reachable as \\.\pipe\C:\dir\file.sock,
// which is how the herdr agents connection below works from plain Node.
const IS_WIN = process.platform === 'win32';
const pipePrefix = IS_WIN ? '\\\\.\\pipe\\' : '';

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
    // Persistent PowerShell workers (GPU / Bluetooth), keyed by module. `gen`
    // invalidates stale exit handlers: without it a worker killed by stop()
    // respawns alongside the fresh one start() just spawned, because the
    // killed child's exit event only fires after setConfig reset _stopping.
    this._wk = { gpu: { proc: null, gen: 0 }, bt: { proc: null, gen: 0 } };
    this._respawnTimers = [];
    this._agentsFileTimer = null;
    this._agentsLive = false;
    this._agentsSock = null;
    this._agentsMap = new Map();
    this._agentsBackoff = 0;
    this._gpuBuf = '';
    this._btBuf = '';
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
    if (m.agents && m.agents.enabled) this._startAgentsLive();
  }

  stop() {
    this._stopping = true;
    for (const t of this._timers) clearInterval(t);
    this._timers = [];
    for (const t of this._respawnTimers) clearTimeout(t);
    this._respawnTimers = [];
    if (this._agentsFileTimer) { clearInterval(this._agentsFileTimer); this._agentsFileTimer = null; }
    if (this._agentsRefresh) { clearInterval(this._agentsRefresh); this._agentsRefresh = null; }
    if (this._agentsReconnect) { clearTimeout(this._agentsReconnect); this._agentsReconnect = null; }
    if (this._agentsSock) { try { this._agentsSock.destroy(); } catch (_) {} this._agentsSock = null; }
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
  // Live source: the herdr server socket. On Windows an AF_UNIX socket bound at
  // <path> is reachable as the named pipe \\.\pipe\<path>, so the main process
  // connects directly - no worker process, no polling. The server answers a
  // session.snapshot by CLOSING the connection, so state comes from one-shot
  // snapshot connections while one persistent connection carries push events
  // (per-pane agent status changes). Real time, no orchestrator middleman.
  // While herdr is unavailable the chip falls back to the status file.
  _startAgentsLive() {
    this._agentsMap = new Map(); // pane_id -> agent_status
    this._agentsSubbed = new Set(); // pane_ids with per-pane subscriptions
    this._agentsSnapBusy = false;
    this._agentsConnectEvents();
    this._agentsSnapshotOnce();
    // Slow refresh heals anything a missed event could leave stale.
    this._agentsRefresh = setInterval(() => this._agentsSnapshotOnce(), 60000);
  }

  _agentsConnectEvents() {
    if (this._stopping || this._agentsSock) return;
    const s = net.connect({ path: pipePrefix + this._agentsSockPath() });
    this._agentsSock = s;
    s.setEncoding('utf8');
    let buf = '';
    s.on('connect', () => {
      this._agentsBackoff = 0;
      this._agentsSubbed = new Set();
      const subs = [
        { type: 'pane.agent_detected' },
        { type: 'pane.created' }
      ];
      for (const p of this._agentsMap.keys()) subs.push(...this._agentsPaneSubs(p));
      this._agentsSockWrite(s, { id: 'sub', method: 'events.subscribe', params: { subscriptions: subs } });
    });
    s.on('data', (chunk) => {
      buf += chunk;
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, i).trim();
        buf = buf.slice(i + 1);
        if (!line) continue;
        this._agentsOnEvent(line);
      }
    });
    const retry = (err) => {
      if (this._agentsSock !== s) return;
      this._agentsSock = null;
      if (this._stopping) return;
      if (err && err.code) console.error(`[wizbar] agents: herdr events connect failed (${err.code}), retrying`);
      this._agentsUseFile(); // legacy source armed while herdr is unreachable
      this._agentsBackoff = Math.min((this._agentsBackoff || 0) + 4000, 30000);
      this._agentsReconnect = setTimeout(() => this._agentsConnectEvents(), this._agentsBackoff);
    };
    s.on('error', retry);
    s.on('close', retry);
  }

  _agentsPaneSubs(paneId) {
    return [
      { type: 'pane.agent_status_changed', pane_id: paneId },
      { type: 'pane.exited', pane_id: paneId },
      { type: 'pane.closed', pane_id: paneId }
    ];
  }

  // Socket path: modules.agents.sockPath overrides. Windows default is
  // %APPDATA%\herdr\herdr.sock (an AF_UNIX path served over the named-pipe
  // filesystem); other platforms default to the XDG data location.
  _agentsSockPath() {
    const override = this.cfg.modules.agents && this.cfg.modules.agents.sockPath;
    if (override) return String(override).replace(/^~/, os.homedir());
    if (IS_WIN) return path.join(process.env.APPDATA || '', 'herdr', 'herdr.sock');
    return path.join(os.homedir(), '.local', 'share', 'herdr', 'herdr.sock');
  }

  // One-shot state fetch: connect, ask, read one line. The server closes the
  // connection after answering - that is its normal CLI flow, not an error.
  _agentsSnapshotOnce() {
    if (this._stopping || this._agentsSnapBusy) return;
    this._agentsSnapBusy = true;
    const s = net.connect({ path: pipePrefix + this._agentsSockPath() });
    s.setEncoding('utf8');
    let buf = '';
    let got = false;
    s.setTimeout(5000);
    s.on('connect', () => {
      this._agentsBackoff = 0;
      this._agentsSockWrite(s, { id: 'snap', method: 'session.snapshot', params: {} });
    });
    s.on('data', (chunk) => {
      buf += chunk;
      const i = buf.indexOf('\n');
      if (i < 0 || got) return;
      got = true;
      this._agentsApplySnapshot(buf.slice(0, i));
      s.destroy();
    });
    s.on('timeout', () => s.destroy());
    s.on('error', (err) => {
      if (err.code) console.error(`[wizbar] agents: herdr snapshot failed (${err.code})`);
      this._agentsUseFile(); // legacy source armed while herdr is unreachable
    });
    s.on('close', () => { this._agentsSnapBusy = false; });
  }

  _agentsSockWrite(s, obj) {
    try { s.write(JSON.stringify(obj) + '\n'); } catch (_) {}
  }

  _agentsApplySnapshot(line) {
    try {
      const d = JSON.parse(line);
      const ag = (d.result && d.result.snapshot && d.result.snapshot.agents) || [];
      this._agentsMap = new Map(ag.map((a) => [a.pane_id, a.agent_status || 'unknown']));
      this._agentsEmit();
      // Subscribe per-pane on the event channel for panes we have not subbed.
      if (this._agentsSock) {
        const fresh = [...this._agentsMap.keys()].filter((p) => !this._agentsSubbed.has(p));
        if (fresh.length) {
          for (const p of fresh) this._agentsSubbed.add(p);
          const subs = fresh.flatMap((p) => this._agentsPaneSubs(p));
          this._agentsSockWrite(this._agentsSock, { id: 'sub-panes', method: 'events.subscribe', params: { subscriptions: subs } });
        }
      }
    } catch (_) {}
  }

  _agentsOnEvent(line) {
    try {
      const d = JSON.parse(line);
      const ev = d.type || '';
      if (ev === 'pane.agent_status_changed') {
        if (d.agent_status) this._agentsMap.set(d.pane_id, d.agent_status);
        else this._agentsMap.delete(d.pane_id);
        this._agentsEmit();
      } else if (ev === 'pane.agent_detected' || ev === 'pane.created' || ev === 'pane.closed' || ev === 'pane.exited') {
        if (ev === 'pane.closed' || ev === 'pane.exited') this._agentsMap.delete(d.pane_id);
        // New agent: its current status is unknown to us - refresh from a
        // snapshot (which also subscribes the new pane's status changes).
        this._agentsSnapshotOnce();
      }
    } catch (_) {}
  }

  _agentsEmit() {
    const entries = [...this._agentsMap.values()];
    // blocked still means the agent needs its captain - count it as live.
    const working = entries.filter((st) => st === 'working' || st === 'blocked').length;
    const total = entries.length;
    const note = total === 0 ? 'herdr reachable, no agent panes' : `${working}/${total} working`;
    const line = JSON.stringify({ state: working > 0 ? 'working' : 'idle', count: total, working, note });
    if (line === this._agentsLastLine) return;
    this._agentsLastLine = line;
    this._agentsLive = true;
    this._agentsStopFile();
    this.state.agents = JSON.parse(line);
  }

  // Legacy source: the orchestrator's hand-written fleet-status.json. Only used
  // when the live herdr path is unavailable (herdr not running).
  _agentsUseFile() {
    if (this._agentsFileTimer) return;
    console.error('[wizbar] agents: herdr live source unavailable, falling back to status file');
    const m = this.cfg.modules.agents;
    this._agentsFileTimer = setInterval(() => this._pollAgents(), Math.max(2000, (m && m.intervalMs) || 5000));
    this._pollAgents();
  }

  _agentsStopFile() {
    if (this._agentsFileTimer) { clearInterval(this._agentsFileTimer); this._agentsFileTimer = null; }
  }

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
    this._spawnWorker('gpu', 'powershell.exe', script, (line) => {
      try {
        const d = JSON.parse(line);
        if (d.err) { if (!this.state.gpu) this.state.gpu = { error: 'counter' }; return; }
        this.state.gpu = {
          [mode]: Math.min(100, Math.round(d[mode])),
          sum: Math.min(100, Math.round(d.sum)),
          max: Math.min(100, Math.round(d.max))
        };
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
    return { ...this.state, now: new Date().toISOString() };
  }
}

module.exports = { MetricsEngine };
