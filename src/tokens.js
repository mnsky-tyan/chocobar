'use strict';
// Cross-CLI token usage scanner.
//
// Sources (all local, read-only):
//  - zcode:  ~/.zcode/cli/db/db.sqlite  -> `turn_usage` table (per-turn token totals).
//            Durable history for every model call the zcode client makes.
//  - zai:    two stores. DB turns whose session id appears as
//            ~/.zai/agent/sessions/ZCODE_sess_<uuid>_*.jsonl were launched from the
//            pre-rebuild zai wrapper and stay attributed via the DB. The rebuilt
//            pi-based engine (2026-09-10) keeps transcripts named <utc-ts>_<uuid>.jsonl
//            whose ids never reach the zcode DB; their assistant messages carry a
//            `usage` object, which we scan directly.
//  - opencode: ~/.local/share/opencode/storage/message/<session>/msg_*.json
//            assistant messages carry a `tokens` object.
//  - mimo:  Xiaomi MiMo AI desktop exposes a localhost HTTP API while it runs
//            (port + bearer token in %APPDATA%\Xiaomi MiMo AI\desktop-api.json);
//            GET /v1/sessions and /v1/sessions/<id>/messages return transcripts
//            whose assistant messages carry a `tokens` object. History persists
//            in our own token cache — when the app is closed nothing new scans.
//
// The sources themselves are the durable store; we keep an in-memory record map
// keyed by a stable source id (dedup) and a small cache file for fast restarts.
const fs = require('fs');
const path = require('path');
const os = require('os');
const { execFile } = require('child_process');
const { APP_DIR } = require('./config');

const CACHE_PATH = path.join(APP_DIR, 'token-cache.json');
const DAY_MS = 86400000;

function localDateKey(tsMs) {
  const d = new Date(tsMs);
  const m = String(d.getMonth() + 1).padStart(2, '0');
  const day = String(d.getDate()).padStart(2, '0');
  return `${d.getFullYear()}-${m}-${day}`;
}

function emptyAgg() {
  return { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, reasoning: 0, requests: 0 };
}

function addAgg(a, r) {
  a.input += r.input || 0;
  a.output += r.output || 0;
  a.cacheRead += r.cacheRead || 0;
  a.cacheWrite += r.cacheWrite || 0;
  a.reasoning += r.reasoning || 0;
  a.requests += 1;
}

class TokenTracker extends require('events') {
  constructor(config) {
    super();
    this.cfg = config.tokens;
    this.records = new Map();   // key -> { app, ts, model, input, output, cacheRead, cacheWrite, reasoning }
    this.lastScan = null;
    this._timer = null;
    this._pythonCmd = 'python'; // may be re-probed to python3 on Linux
    if (this.cfg.enabled) this._loadCache(); // master off: nothing is read at all
  }

  start() {
    clearInterval(this._timer);
    this._timer = null;
    if (!this.cfg.enabled) return; // master off: no scan timer, zero scans
    this._timer = setInterval(() => this.rescan(), Math.max(1, this.cfg.rescanMinutes) * 60000);
    this.rescan();
  }

  // Scan all sources now. Concurrent callers share one in-flight scan (a
  // manual refresh during the periodic tick queues behind it). `full` drops
  // the zai/opencode mtime cursors so every source file is re-read — the
  // record-key dedup makes that safe, and it recovers anything a stale cursor
  // skipped.
  rescan({ full = false } = {}) {
    if (!this.cfg.enabled) return Promise.resolve(this.aggregate()); // master off: no scan
    if (!this._scanPromise) {
      this._scanPromise = this._runScan(full).finally(() => { this._scanPromise = null; });
    }
    return this._scanPromise;
  }

  async _runScan(full) {
    if (full) {
      this._zaiMtimeFloor = 0; this._zaiMtimeHigh = 0;
      this._piMtimeFloor = 0; this._piMtimeHigh = 0;
      this._ocMtimeFloor = 0; this._ocMtimeHigh = 0;
    }
    const t0 = Date.now();
    let added = 0;
    try { added += await this._scanZcode(); } catch (e) { console.error('[wizbar] zcode scan:', e.message); }
    try { added += this._scanZaiSessions(); } catch (e) { console.error('[wizbar] zai scan:', e.message); }
    try { added += this._scanPiAgentSessions(); } catch (e) { console.error('[wizbar] pi scan:', e.message); }
    try { added += this._scanOpencode(); } catch (e) { console.error('[wizbar] opencode scan:', e.message); }
    try { added += await this._scanMimo(); } catch (e) { console.error('[wizbar] mimo scan:', e.message); }
    this.lastScan = new Date().toISOString();
    this._saveCache();
    this.emit('updated', this.aggregate());
    if (added) console.log(`[wizbar] tokens: +${added} records (${Date.now() - t0}ms)`);
    return this.aggregate();
  }

  setConfig(config) {
    this.cfg = config.tokens;
    this.start();
  }

  // --- zcode (+zai) -----------------------------------------------------------
  _zaiSessionIds(sessionsDir) {
    const ids = new Set();
    try {
      for (const f of fs.readdirSync(sessionsDir)) {
        const m = f.match(/^ZCODE_sess_([0-9a-f-]{36})_/i);
        if (m) ids.add(m[1].toLowerCase());
      }
    } catch (_) {}
    return ids;
  }

  // Async spawn: at a 1-minute scan cadence a synchronous execFileSync would
  // freeze the main process (bar follow loop included) for the length of the
  // python run every scan. Interpreter name is platform-dependent: Windows
  // installs usually provide `python`, Debian/Ubuntu often only `python3`.
  async _scanZcode() {
    const src = this.cfg.sources.zcode;
    if (!src.enabled || !src.dbPath || !fs.existsSync(src.dbPath)) return 0;
    const zaiIds = this._zaiSessionIds(this.cfg.sources.zai.sessionsDir);
    const script = path.join(__dirname, '..', 'scripts', 'zcode_query.py');
    const run = (py) => new Promise((resolve, reject) => {
      execFile(py, [script, src.dbPath], {
        windowsHide: true,
        maxBuffer: 64 * 1024 * 1024,
        encoding: 'utf8'
      }, (err, out) => (err ? reject(err) : resolve(out)));
    });
    let stdout;
    try {
      stdout = await run(this._pythonCmd);
    } catch (e) {
      if (e && e.code === 'ENOENT' && this._pythonCmd !== 'python3') {
        this._pythonCmd = 'python3'; // remember the working interpreter
        stdout = await run('python3');
      } else {
        throw e;
      }
    }
    const rows = JSON.parse(stdout);
    let added = 0;
    for (const r of rows) {
      const key = `z:${r.session}:${r.turn}`;
      if (this.records.has(key)) continue;
      const sessUuid = String(r.session || '').replace(/^sess_/, '').toLowerCase();
      this.records.set(key, {
        app: zaiIds.has(sessUuid) ? 'zai' : 'zcode',
        ts: Number(r.ts),
        model: r.model || 'unknown',
        input: r.input || 0,
        output: r.output || 0,
        reasoning: r.reasoning || 0,
        cacheRead: r.cacheRead || 0,
        cacheWrite: r.cacheWrite || 0
      });
      added++;
    }
    return added;
  }

  // --- pi-format session stores (zai + pi) ------------------------------------
  // Both the rebuilt zai engine and the standalone pi coding agent persist
  // JSONL transcripts whose assistant messages carry a `usage` object:
  //   {type:"message", message:{role:"assistant", usage:{input,output,
  //    cacheRead,cacheWrite}, model, timestamp}}
  // zai keeps them FLAT in one folder; pi nests them under per-project
  // subfolders of ~/.pi/agent/sessions. One scanner serves both.
  // Legacy ZCODE_* files are skipped: those sessions are DB-backed (turn_usage),
  // so counting the files too would double them.
  _listSessionFiles(rootDir, depth) {
    const files = [];
    let entries;
    try { entries = fs.readdirSync(rootDir, { withFileTypes: true }); } catch (_) { return files; }
    for (const e of entries) {
      const full = path.join(rootDir, e.name);
      if (e.isDirectory()) {
        if (depth > 0) files.push(...this._listSessionFiles(full, depth - 1));
      } else if (e.isFile() && e.name.endsWith('.jsonl') && !e.name.startsWith('ZCODE_')) {
        files.push(full);
      }
    }
    return files;
  }

  _scanPiSessions(source, app, keyPrefix, floorKey, highKey) {
    if (!source || !source.enabled || !source.sessionsDir) return 0;
    const files = this._listSessionFiles(source.sessionsDir, 2);
    const cutoff = (this[floorKey] || 0);
    let added = 0, high = (this[highKey] || 0);
    for (const full of files) {
      let st;
      try { st = fs.statSync(full); } catch (_) { continue; }
      if (st.mtimeMs < cutoff) continue;
      if (st.mtimeMs > high) high = st.mtimeMs;
      // Key on the path relative to the store root: stable across rescans,
      // unique across nested project folders.
      const rel = path.relative(source.sessionsDir, full);
      let lines;
      try { lines = fs.readFileSync(full, 'utf8').split('\n'); } catch (_) { continue; }
      for (let i = 0; i < lines.length; i++) {
        const line = lines[i].trim();
        if (!line.includes('"usage"')) continue;
        let d;
        try { d = JSON.parse(line); } catch (_) { continue; } // partial tail line while appending
        if (d.type !== 'message' || !d.message || d.message.role !== 'assistant') continue;
        const u = d.message.usage || {};
        const input = u.input || 0, output = u.output || 0;
        const cacheRead = u.cacheRead || 0, cacheWrite = u.cacheWrite || 0;
        if (!(input || output || cacheRead || cacheWrite)) continue;
        const key = `${keyPrefix}:${rel}:${(d.message && d.message.id) || 'l' + i}`;
        if (this.records.has(key)) continue;
        this.records.set(key, {
          app,
          ts: Number(d.message.timestamp) || st.mtimeMs,
          model: d.message.model || 'unknown',
          // pi reports cache beside input (its input EXCLUDES cache, unlike the
          // zcode DB) and its own totalTokens = input+output+cacheRead+cacheWrite;
          // folding cache into input makes the record cache-inclusive like DB rows.
          input: input + cacheRead + cacheWrite, output, cacheRead, cacheWrite, reasoning: 0
        });
        added++;
      }
    }
    if (high) this[highKey] = high;
    if (this[highKey]) this[floorKey] = this[highKey] - 60000; // 1min slack for in-flight writes
    return added;
  }

  _scanZaiSessions() {
    return this._scanPiSessions(this.cfg.sources.zai, 'zai', 'zf', '_zaiMtimeFloor', '_zaiMtimeHigh');
  }

  _scanPiAgentSessions() {
    return this._scanPiSessions(this.cfg.sources.pi, 'pi', 'pf', '_piMtimeFloor', '_piMtimeHigh');
  }

  // --- opencode ----------------------------------------------------------------
  _scanOpencode() {
    const src = this.cfg.sources.opencode;
    if (!src.enabled || !src.storageDir || !fs.existsSync(src.storageDir)) return 0;
    let added = 0;
    const cutoff = (this._ocMtimeFloor || 0);
    const dirs = fs.readdirSync(src.storageDir, { withFileTypes: true })
      .filter((d) => d.isDirectory());
    for (const d of dirs) {
      const dir = path.join(src.storageDir, d.name);
      let files;
      try { files = fs.readdirSync(dir); } catch (_) { continue; }
      for (const fn of files) {
        if (!fn.startsWith('msg_') || !fn.endsWith('.json')) continue;
        const full = path.join(dir, fn);
        let st;
        try { st = fs.statSync(full); } catch (_) { continue; }
        if (st.mtimeMs < cutoff) continue;
        let msg;
        try { msg = JSON.parse(fs.readFileSync(full, 'utf8')); } catch (_) { continue; }
        const t = msg.tokens;
        if (!t || msg.role !== 'assistant') continue;
        const key = `o:${msg.id || fn}`;
        if (this.records.has(key)) continue;
        this.records.set(key, {
          app: 'opencode',
          ts: (msg.time && msg.time.created) || st.mtimeMs,
          model: (msg.model && (msg.model.modelID || msg.model.modelId)) || 'unknown',
          // Same normalization as the pi scan: opencode's input excludes cache, and
          // reasoning is a breakdown of output (never additive).
          input: (t.input || 0) + ((t.cache && t.cache.read) || 0) + ((t.cache && t.cache.write) || 0),
          output: t.output || 0,
          reasoning: t.reasoning || 0,
          cacheRead: (t.cache && t.cache.read) || 0,
          cacheWrite: (t.cache && t.cache.write) || 0
        });
        added++;
        if (st.mtimeMs > (this._ocMtimeHigh || 0)) this._ocMtimeHigh = st.mtimeMs;
      }
    }
    if (this._ocMtimeHigh) this._ocMtimeFloor = this._ocMtimeHigh - 60000; // 1min slack for in-flight writes
    return added;
  }

  // --- Xiaomi MiMo AI desktop ---------------------------------------------------
  // Assistant messages from the app's local API carry
  // tokens {input, output, reasoning, cache:{read,write}} where input EXCLUDES
  // cache (total = input+output+cacheRead+cacheWrite) — same shape as
  // pi/opencode, so cache folds into the stored input the same way.
  async _scanMimo() {
    const src = this.cfg.sources.mimo;
    if (!src || !src.enabled) return 0;
    const apiFile = path.join(process.env.APPDATA || '', 'Xiaomi MiMo AI', 'desktop-api.json');
    let api;
    try { api = JSON.parse(fs.readFileSync(apiFile, 'utf8')); } catch (_) { return 0; } // app not running
    if (!api || !api.port || !api.token) return 0;
    const base = `http://127.0.0.1:${api.port}`;
    const headers = { Authorization: 'Bearer ' + api.token };
    const sessions = await this._mimoFetch(base + '/v1/sessions?limit=200', headers);
    if (!Array.isArray(sessions)) return 0;
    if (!this._mimoSigs) this._mimoSigs = {};
    let added = 0;
    for (const s of sessions) {
      // Skip sessions whose last-update stamp already scanned: keeps the poll
      // cheap even though every fetch returns the whole transcript payload.
      const sig = s.time && s.time.updated;
      if (sig != null && this._mimoSigs[s.id] === sig) continue;
      const msgs = await this._mimoFetch(`${base}/v1/sessions/${encodeURIComponent(s.id)}/messages`, headers);
      if (Array.isArray(msgs)) {
        for (const m of msgs) {
          const info = m && m.info;
          const t = info && info.tokens;
          if (!info || info.role !== 'assistant' || !t) continue;
          const input = t.input || 0, output = t.output || 0;
          const cacheRead = (t.cache && t.cache.read) || 0, cacheWrite = (t.cache && t.cache.write) || 0;
          if (!(input || output || cacheRead || cacheWrite)) continue;
          const key = `m:${info.id}`;
          if (this.records.has(key)) continue;
          this.records.set(key, {
            app: 'mimo',
            ts: (info.time && info.time.created) || 0,
            model: info.modelID || 'unknown',
            input: input + cacheRead + cacheWrite,
            output,
            reasoning: t.reasoning || 0,
            cacheRead, cacheWrite
          });
          added++;
        }
      }
      if (sig != null) this._mimoSigs[s.id] = sig;
    }
    return added;
  }

  _mimoFetch(url, headers) {
    return fetch(url, { headers, signal: AbortSignal.timeout(5000) })
      .then((r) => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); });
  }

  // --- cache ---------------------------------------------------------------------
  _loadCache() {
    try {
      if (!fs.existsSync(CACHE_PATH)) return;
      const parsed = JSON.parse(fs.readFileSync(CACHE_PATH, 'utf8'));
      // v2 wraps entries as {entries}; a bare array is a pre-normalization cache
      // whose records lack the folded input - discard it so the next scan is full.
      const arr = Array.isArray(parsed) ? null : parsed.entries;
      if (arr) for (const [k, r] of arr) this.records.set(k, r);
      else console.log('[wizbar] token cache: stale pre-normalization cache discarded; full rescan');
      if (parsed.mimoSigs) this._mimoSigs = parsed.mimoSigs;
      if (arr) this._ocMtimeFloor = Math.max(...arr.map(([, r]) => r.ts || 0));
      if (arr) this._zaiMtimeFloor = Math.max(0, ...arr.filter(([k]) => k.startsWith('zf:')).map(([, r]) => r.ts || 0));
      if (arr) this._piMtimeFloor = Math.max(0, ...arr.filter(([k]) => k.startsWith('pf:')).map(([, r]) => r.ts || 0));
      console.log(`[wizbar] token cache: ${this.records.size} records`);
    } catch (_) {}
  }

  _saveCache() {
    try {
      const arr = [...this.records.entries()].slice(-50000);
      fs.writeFileSync(CACHE_PATH, JSON.stringify({ v: 2, entries: arr, mimoSigs: this._mimoSigs || {} }), 'utf8');
    } catch (e) {
      console.error('[wizbar] token cache write failed:', e.message);
    }
  }

  // --- aggregation -----------------------------------------------------------------
  // Totals count input + output only. Every scan site stores a cache-INCLUSIVE
  // input: zcode DB input_tokens already include cached tokens, while pi and
  // opencode report cache beside input and their scans fold it in, so
  // input+output is the provider total for every source. Adding the cache
  // columns to the total would double-count them; they stay as detail only.
  aggregate() {
    if (!this.cfg.enabled) {
      // Master off: no sources are running, so the only honest dashboard state
      // is all zeros with nothing enabled.
      return {
        generatedAt: new Date().toISOString(), lastScan: null,
        today: { total: 0, apps: {} },
        week: 0, month: 0, allTime: 0,
        byDay: {}, byApp: {}, byModel: {},
        recordCount: 0, heatmapWeeks: this.cfg.heatmapWeeks, sourcesEnabled: 0
      };
    }
    const rowTotal = (r) => (r.input || 0) + (r.output || 0);
    const byDay = new Map();     // dateKey -> { total, apps: { app: agg } }
    const byApp = new Map();     // app -> agg
    const byModel = new Map();   // "app|lowercased model" -> agg
    const modelVariants = new Map(); // byModel key -> Map(raw model id -> record count)
    let allTime = 0;

    for (const r of this.records.values()) {
      const dk = localDateKey(r.ts);
      if (!byDay.has(dk)) byDay.set(dk, { total: 0, apps: {} });
      const day = byDay.get(dk);
      if (!day.apps[r.app]) day.apps[r.app] = emptyAgg();
      addAgg(day.apps[r.app], r);
      day.total += rowTotal(r);
      allTime += rowTotal(r);

      if (!byApp.has(r.app)) byApp.set(r.app, emptyAgg());
      addAgg(byApp.get(r.app), r);
      // Same model can be recorded under different casing ("GLM-5.3-Flash" vs
      // "glm-5.3-flash" — the casing flips between sources and even
      // mid-session), so the grouping key is case-insensitive; the raw
      // variants are tallied to pick one display label below.
      const model = r.model || 'unknown';
      const mk = `${r.app}|${model.toLowerCase()}`;
      if (!byModel.has(mk)) byModel.set(mk, emptyAgg());
      addAgg(byModel.get(mk), r);
      let variants = modelVariants.get(mk);
      if (!variants) modelVariants.set(mk, (variants = new Map()));
      variants.set(model, (variants.get(model) || 0) + 1);
    }

    // Display label per merged group: the most frequent raw variant; ties
    // break deterministically on codepoint order (so "GLM-5.3-Flash" wins over
    // "glm-5.3-flash" on an even split).
    for (const [mk, group] of byModel) {
      const ranked = [...modelVariants.get(mk).entries()]
        .sort((a, b) => b[1] - a[1] || (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
      group.modelLabel = ranked[0][0];
    }

    const todayKey = localDateKey(Date.now());
    const today = byDay.get(todayKey) || { total: 0, apps: {} };
    const weekAgo = Date.now() - 7 * DAY_MS;
    const monthAgo = Date.now() - 30 * DAY_MS;
    let week = 0, month = 0;
    for (const r of this.records.values()) {
      const t = rowTotal(r);
      if (r.ts >= weekAgo) week += t;
      if (r.ts >= monthAgo) month += t;
    }

    return {
      generatedAt: new Date().toISOString(),
      lastScan: this.lastScan,
      today: { total: today.total, apps: today.apps },
      week, month, allTime,
      byDay: Object.fromEntries([...byDay.entries()].sort((a, b) => a[0].localeCompare(b[0]))),
      byApp: Object.fromEntries(byApp),
      byModel: Object.fromEntries(byModel),
      recordCount: this.records.size,
      heatmapWeeks: this.cfg.heatmapWeeks,
      // How many usage sources are switched on — lets the dashboard explain an
      // empty state ("no sources configured") instead of just showing zeros.
      sourcesEnabled: Object.values(this.cfg.sources || {}).filter((s) => s && s.enabled).length
    };
  }
}

module.exports = { TokenTracker, localDateKey };
