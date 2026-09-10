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
//
// The sources themselves are the durable store; we keep an in-memory record map
// keyed by a stable source id (dedup) and a small cache file for fast restarts.
const fs = require('fs');
const path = require('path');
const os = require('os');
const { execFile, execFileSync } = require('child_process');
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
    this._loadCache();
  }

  start() {
    const run = async () => {
      const t0 = Date.now();
      let added = 0;
      try { added += this._scanZcode(); } catch (e) { console.error('[wizbar] zcode scan:', e.message); }
      try { added += this._scanZaiSessions(); } catch (e) { console.error('[wizbar] zai scan:', e.message); }
      try { added += this._scanOpencode(); } catch (e) { console.error('[wizbar] opencode scan:', e.message); }
      this.lastScan = new Date().toISOString();
      this._saveCache();
      this.emit('updated', this.aggregate());
      if (added) console.log(`[wizbar] tokens: +${added} records (${Date.now() - t0}ms)`);
    };
    run();
    clearInterval(this._timer);
    this._timer = setInterval(run, Math.max(1, this.cfg.rescanMinutes) * 60000);
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

  _scanZcode() {
    const src = this.cfg.sources.zcode;
    if (!src.enabled || !src.dbPath || !fs.existsSync(src.dbPath)) return 0;
    const zaiIds = this._zaiSessionIds(this.cfg.sources.zai.sessionsDir);
    const script = path.join(__dirname, '..', 'scripts', 'zcode_query.py');
    const rows = JSON.parse(execFileSync('python', [script, src.dbPath], {
      windowsHide: true,
      maxBuffer: 64 * 1024 * 1024,
      encoding: 'utf8'
    }));
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

  // --- zai (pi agent sessions) ---------------------------------------------------
  // Legacy ZCODE_* files are skipped: those sessions are DB-backed (turn_usage), so
  // counting the files too would double them. New-format files are the only store
  // for post-rebuild zai usage.
  _scanZaiSessions() {
    const src = this.cfg.sources.zai;
    if (!src.enabled || !src.sessionsDir) return 0;
    let files;
    try { files = fs.readdirSync(src.sessionsDir); } catch (_) { return 0; }
    const cutoff = (this._zaiMtimeFloor || 0);
    let added = 0, high = (this._zaiMtimeHigh || 0);
    for (const fn of files) {
      if (!fn.endsWith('.jsonl') || fn.startsWith('ZCODE_')) continue;
      const full = path.join(src.sessionsDir, fn);
      let st;
      try { st = fs.statSync(full); } catch (_) { continue; }
      if (st.mtimeMs < cutoff) continue;
      if (st.mtimeMs > high) high = st.mtimeMs;
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
        const key = `zf:${fn}:${(d.message && d.message.id) || 'l' + i}`;
        if (this.records.has(key)) continue;
        this.records.set(key, {
          app: 'zai',
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
    if (high) this._zaiMtimeHigh = high;
    if (this._zaiMtimeHigh) this._zaiMtimeFloor = this._zaiMtimeHigh - 60000; // 1min slack for in-flight writes
    return added;
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
      if (arr) this._ocMtimeFloor = Math.max(...arr.map(([, r]) => r.ts || 0));
      if (arr) this._zaiMtimeFloor = Math.max(0, ...arr.filter(([k]) => k.startsWith('zf:')).map(([, r]) => r.ts || 0));
      console.log(`[wizbar] token cache: ${this.records.size} records`);
    } catch (_) {}
  }

  _saveCache() {
    try {
      const arr = [...this.records.entries()].slice(-50000);
      fs.writeFileSync(CACHE_PATH, JSON.stringify({ v: 2, entries: arr }), 'utf8');
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
    const rowTotal = (r) => (r.input || 0) + (r.output || 0);
    const byDay = new Map();     // dateKey -> { total, apps: { app: agg } }
    const byApp = new Map();     // app -> agg
    const byModel = new Map();   // "app|model" -> agg
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
      const mk = `${r.app}|${r.model || 'unknown'}`;
      if (!byModel.has(mk)) byModel.set(mk, emptyAgg());
      addAgg(byModel.get(mk), r);
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
      heatmapWeeks: this.cfg.heatmapWeeks
    };
  }
}

module.exports = { TokenTracker, localDateKey };
