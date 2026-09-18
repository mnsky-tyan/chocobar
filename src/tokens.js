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
//  - pi:     ~/.pi/agent/sessions/<project-slug>/*.jsonl — the standalone pi
//            coding agent's transcripts, nested under per-project subfolders;
//            same per-message `usage` shape, scanned by the same reader as zai.
//  - opencode: SQLite ~/.local/share/opencode/opencode.db (message.data JSON
//            with a `tokens` object); the legacy storage/message file tree is
//            a fallback. A second WSL-distro store is copied in read-only and
//            counted as its own app ("opencode-wsl").
//  - mimo:  Xiaomi MiMo AI desktop exposes a localhost HTTP API while it runs
//            (port + bearer token in %APPDATA%\Xiaomi MiMo AI\desktop-api.json);
//            GET /v1/sessions and /v1/sessions/<id>/messages return transcripts
//            whose assistant messages carry a `tokens` object. History persists
//            in our own token cache — when the app is closed nothing new scans.
//  - subscription: plan CREDITS for subscription plans (quota-based, not
//            per-message tokens). The user points tokens.sources.subscription
//            at a small JSON file (usagePath) kept anywhere:
//              { "plans": [ { "name": "Pro Plan", "total": 1500,
//                             "used": 430, "resetsAt": "2026-10-14" } ] }
//            Re-read on every rescan; rendered as its own dashboard card.
//
// Token convention: every stored record counts RAW tokens. input and output
// are cache-EXCLUSIVE (what the provider bills as base prompt + completion);
// cacheRead / cacheWrite are stored as separate breakdown columns and are
// NEVER added to a total. Evidence: the zcode DB row
// computed_total_tokens == input_tokens + output_tokens + reasoning_tokens
// exactly, so input_tokens already contains its cache read/write tokens
// (subtracted here to get the raw value); pi/zai transcripts expose
// usage.totalTokens == input + output + cacheRead + cacheWrite, so their
// input excludes cache already (stored as-is).
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

function expandHome(p) {
  if (typeof p === 'string' && p.startsWith('~/')) {
    return path.join(os.homedir(), p.slice(2));
  }
  return p;
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
    this.subscriptionPlans = null; // parsed tokens.sources.subscription file, null = off/unreadable
    this._sigsDirty = false;      // mimo cursors changed since the last cache save
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
      this._fileProgress = new Map(); // byte cursors too: re-read everything (dedup keeps totals)
    }
    const t0 = Date.now();
    let added = 0;
    try { added += await this._scanZcode(); } catch (e) { console.error('[wizbar] zcode scan:', e.message); }
    try { added += this._scanZaiSessions(); } catch (e) { console.error('[wizbar] zai scan:', e.message); }
    try { added += this._scanPiAgentSessions(); } catch (e) { console.error('[wizbar] pi scan:', e.message); }
    try { added += this._scanOpencode(); } catch (e) { console.error('[wizbar] opencode scan:', e.message); }
    try { added += await this._scanMimo(); } catch (e) { console.error('[wizbar] mimo scan:', e.message); }
    let plansChanged = false;
    try { plansChanged = this._scanSubscription(); } catch (e) { console.error('[wizbar] subscription scan:', e.message); }
    this.lastScan = new Date().toISOString();
    // The cache is a rewrite of up to 50k records; doing that synchronously on
    // every scan (even with zero new records) was both wasteful and a main-
    // process stall. Write only when something actually changed.
    if (added > 0 || this._sigsDirty || plansChanged) {
      this._sigsDirty = false;
      this._saveCache();
    }
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
        // The DB's input_tokens already INCLUDES cache creation/read tokens
        // (computed_total_tokens == input + output + reasoning, verified on the
        // live DB), so the raw value subtracts them. Cache stays as detail.
        input: Math.max(0, (r.input || 0) - (r.cacheWrite || 0) - (r.cacheRead || 0)),
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
    if (!this._fileProgress) this._fileProgress = new Map();
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
      try { added += this._readSessionTail(full, st, rel, app, keyPrefix); } catch (_) {}
    }
    if (high) this[highKey] = high;
    if (this[highKey]) this[floorKey] = this[highKey] - 60000; // 1min slack for in-flight writes
    return added;
  }

  // Read only what changed in one session JSONL since the last scan. Session
  // files are APPEND-ONLY and the active one grows huge (hundreds of MB), so
  // re-reading whole files on the main process every scan was a periodic
  // multi-second UI freeze (the bar and the dashboard share this process).
  // We keep a per-file byte cursor (key -> { offset, mtimeMs }): an append is
  // read from the cursor, an unchanged file is not read at all, and a shrunk
  // or same-length-rewritten file is re-read from zero. Keys embed the record
  // id or the absolute byte offset, so re-reads after a restart dedup against
  // the persisted cache exactly like the old line-index keys did. Cursors are
  // persisted in the cache so even a cold start reads only the fresh tail.
  _readSessionTail(full, st, rel, app, keyPrefix) {
    if (!this._fileProgress) this._fileProgress = new Map();
    const prog = this._fileProgress.get(keyPrefix + ':' + rel);
    let start = 0;
    if (prog) {
      if (st.size === prog.offset) {
        if (st.mtimeMs === prog.mtimeMs) return 0;      // nothing appended
        start = 0;                                       // same length, new mtime: rewritten in place
      } else if (st.size > prog.offset) {
        start = prog.offset;                             // normal append
      } // else shrunk/rotated: re-read from zero
    }
    const fd = fs.openSync(full, 'r');
    let buf, read = 0;
    try {
      const len = st.size - start;
      buf = Buffer.allocUnsafe(len);
      while (read < len) {
        const n = fs.readSync(fd, buf, read, len - read, start + read);
        if (n <= 0) break;
        read += n;
      }
    } finally {
      fs.closeSync(fd);
    }
    // Only consume COMPLETE lines: a writer may hold a half-written tail.
    // Everything from the last newline on stays unconsumed for the next scan.
    let usable = read;
    while (usable > 0 && buf[usable - 1] !== 10) usable--;   // trailing partial line
    const partial = usable < read;
    const tailKey = keyPrefix + ':' + rel;
    if (!this._pendingTails) this._pendingTails = new Map();
    const prevTail = this._pendingTails.get(tailKey);
    // A trailing unterminated line is normally a WRITER mid-line: retry next
    // scan. But a FINALIZED file whose last line lacks the trailing newline
    // never grows again - detect it by the tail being unchanged since the
    // previous scan (same start, size and mtime) and count it exactly once.
    const tailSettled = prevTail && prevTail.size === st.size && prevTail.mtimeMs === st.mtimeMs;
    if (usable === 0 && read > 0) {
      if (tailSettled) {
        usable = read; // whole region is one final unterminated line
        this._pendingTails.delete(tailKey);
      } else {
        this._pendingTails.set(tailKey, { offset: start, size: st.size, mtimeMs: st.mtimeMs });
        this._fileProgress.set(tailKey, { offset: start, mtimeMs: st.mtimeMs });
        return 0;
      }
    } else if (partial) {
      if (tailSettled && prevTail.offset === start + usable) {
        usable = read; // the tail is final: consume it fully this time
        this._pendingTails.delete(tailKey);
      } else {
        this._pendingTails.set(tailKey, { offset: start + usable, size: st.size, mtimeMs: st.mtimeMs });
        // complete lines above still process; the cursor stops before the tail
      }
    } else if (prevTail) {
      this._pendingTails.delete(tailKey); // file ended cleanly or grew past it
    }
    let added = 0;
    let lineStart = 0;
    for (let i = 0; i <= usable; i++) {
      if (i !== usable && buf[i] !== 10) continue;
      if (i > lineStart) {
        const line = buf.toString('utf8', lineStart, i).trim();
        if (line.includes('"usage"')) {
          let d;
          try { d = JSON.parse(line); } catch (_) {} // partial/corrupt line: skip
          if (d && d.type === 'message' && d.message && d.message.role === 'assistant') {
            const u = d.message.usage || {};
            const input = u.input || 0, output = u.output || 0;
            const cacheRead = u.cacheRead || 0, cacheWrite = u.cacheWrite || 0;
            if (input || output || cacheRead || cacheWrite) {
              const key = d.message.id
                ? `${keyPrefix}:${rel}:${d.message.id}`
                : `${keyPrefix}:${rel}:b${start + lineStart}`;
              if (!this.records.has(key)) {
                this.records.set(key, {
                  app,
                  ts: Number(d.message.timestamp) || st.mtimeMs,
                  model: d.message.model || 'unknown',
                  // pi's usage.totalTokens == input + output + cacheRead +
                  // cacheWrite (verified on real transcripts), so its input is
                  // already cache-exclusive. Cache columns stay as detail only.
                  input, output, cacheRead, cacheWrite, reasoning: 0
                });
                added++;
              }
            }
          }
        }
      }
      lineStart = i + 1;
    }
    this._fileProgress.set(keyPrefix + ':' + rel, { offset: start + usable, mtimeMs: st.mtimeMs });
    return added;
  }

  _scanZaiSessions() {
    return this._scanPiSessions(this.cfg.sources.zai, 'zai', 'zf', '_zaiMtimeFloor', '_zaiMtimeHigh');
  }

  _scanPiAgentSessions() {
    return this._scanPiSessions(this.cfg.sources.pi, 'pi', 'pf', '_piMtimeFloor', '_piMtimeHigh');
  }

  // --- opencode ----------------------------------------------------------------
  // Prefer the SQLite store (current opencode); fall back to the legacy
  // message-file tree. A second WSL-distro store may be copied in read-only
  // and is counted as its own app ("opencode-wsl") so its numbers never merge
  // into the host store.
  _scanOpencode() {
    const src = this.cfg.sources.opencode;
    if (!src || !src.enabled) return 0;
    let added = 0;
    const dbPath = expandHome(src.dbPath || '~/.local/share/opencode/opencode.db');
    if (dbPath && fs.existsSync(dbPath)) {
      added += this._scanOpencodeDb(dbPath, 'o');
    } else if (src.storageDir && fs.existsSync(expandHome(src.storageDir))) {
      added += this._scanOpencodeFiles();
    }
    // WSL Ubuntu store (same schema, separate machine identity in the key).
    if (src.wsl && src.wsl.enabled !== false) {
      const copy = this._copyWslOpencodeDb(src.wsl);
      if (copy) added += this._scanOpencodeDb(copy, 'ow');
    }
    return added;
  }

  _scanOpencodeFiles() {
    const src = this.cfg.sources.opencode;
    const storageDir = expandHome(src.storageDir);
    if (!fs.existsSync(storageDir)) return 0;
    let added = 0;
    const cutoff = (this._ocMtimeFloor || 0);
    const dirs = fs.readdirSync(storageDir, { withFileTypes: true })
      .filter((d) => d.isDirectory());
    for (const d of dirs) {
      const dir = path.join(storageDir, d.name);
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
          // opencode's input excludes cache (total = input+output+cache):
          // raw input stored as-is, cache is detail.
          input: t.input || 0,
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

  // Scan an opencode SQLite store. message.data is JSON:
  //   { role:'assistant', modelID, providerID, time:{created},
  //     tokens:{ input, output, reasoning, cache:{ read, write } } }
  // keyPrefix 'o' = host store, 'ow' = a WSL-distro copy (own app id).
  _scanOpencodeDb(dbPath, keyPrefix) {
    let DatabaseSync;
    try { ({ DatabaseSync } = require('node:sqlite')); } catch (_) { return 0; }
    let db;
    try { db = new DatabaseSync(dbPath, { readOnly: true }); } catch (e) {
      console.error('[wizbar] opencode db open:', dbPath, e.message);
      return 0;
    }
    let added = 0;
    const app = keyPrefix === 'ow' ? 'opencode-wsl' : 'opencode';
    try {
      const since = keyPrefix === 'ow' ? (this._ocWslHigh || 0) : (this._ocDbHigh || 0);
      const rows = db.prepare(
        `SELECT id, session_id, time_created, data FROM message
         WHERE data LIKE '%"tokens"%' AND time_created >= ?
         ORDER BY time_created`
      ).all(since);
      // session_id -> model label for messages that omit modelID
      const sessionModels = new Map();
      const loadSessionModel = (sid) => {
        if (!sid || sessionModels.has(sid)) return sessionModels.get(sid) || '';
        try {
          const row = db.prepare('SELECT model FROM session WHERE id = ?').get(sid);
          let label = '';
          if (row && row.model) {
            try {
              const mj = typeof row.model === 'string' ? JSON.parse(row.model) : row.model;
              label = mj.id || mj.modelID || '';
            } catch (_) { label = String(row.model); }
          }
          sessionModels.set(sid, label);
          return label;
        } catch (_) {
          sessionModels.set(sid, '');
          return '';
        }
      };
      for (const row of rows) {
        let msg;
        try { msg = JSON.parse(row.data); } catch (_) { continue; }
        if (!msg || msg.role !== 'assistant' || !msg.tokens) continue;
        const t = msg.tokens;
        const input = t.input || 0, output = t.output || 0;
        const cacheRead = (t.cache && t.cache.read) || 0;
        const cacheWrite = (t.cache && t.cache.write) || 0;
        if (!(input || output || cacheRead || cacheWrite)) continue;
        const key = `${keyPrefix}:${row.id}`;
        if (this.records.has(key)) continue;
        const model = msg.modelID
          || (msg.model && (msg.model.modelID || msg.model.modelId))
          || loadSessionModel(row.session_id)
          || 'unknown';
        this.records.set(key, {
          app,
          ts: Number((msg.time && msg.time.created) || row.time_created) || Date.now(),
          model,
          input, // cache-exclusive (opencode reports cache beside input)
          output,
          reasoning: t.reasoning || 0,
          cacheRead,
          cacheWrite
        });
        added++;
        const ts = Number(row.time_created) || 0;
        if (keyPrefix === 'ow') {
          if (ts > (this._ocWslHigh || 0)) this._ocWslHigh = ts;
        } else if (ts > (this._ocDbHigh || 0)) {
          this._ocDbHigh = ts;
        }
      }
    } finally {
      try { db.close(); } catch (_) {}
    }
    if (added) console.log(`[wizbar] opencode ${app}: +${added} records`);
    return added;
  }

  /**
   * Copy the WSL Ubuntu opencode.db so we can open it read-only (SQLite
   * cannot open a live store across the 9p boundary safely). Windows-only:
   * on any other host there is no WSL to call, so nothing is copied.
   */
  _copyWslOpencodeDb(wslCfg) {
    if (process.platform !== 'win32') return null;
    const { execFileSync } = require('child_process');
    const dest = path.join(os.tmpdir(), 'wizbar-oc-copy.db');
    const distro = (wslCfg && wslCfg.distro) || 'Ubuntu';
    const before = (() => { try { return fs.statSync(dest).mtimeMs; } catch (_) { return 0; } })();
    try {
      execFileSync('wsl.exe', ['-d', distro, '--exec', 'bash', path.join(__dirname, '..', 'scripts', 'wsl-opencode-copy.sh')],
        { windowsHide: true, timeout: 60000, encoding: 'utf8',
          env: { ...process.env, WIN_TEMP: path.dirname(dest) } });
      this._ocWslWarned = null;
    } catch (e) {
      if (this._ocWslWarned !== e.message) {
        console.error('[wizbar] wsl opencode copy:', e.message);
        this._ocWslWarned = e.message;
      }
      // A stale copy still beats nothing — WSL is often idle/sleeping.
      if (fs.existsSync(dest)) return dest;
      return null;
    }
    let after = 0;
    try { after = fs.statSync(dest).mtimeMs; } catch (_) {}
    if (!after) return null;
    if (after <= before) {
      // Copy succeeded but the store did not move: force a full re-read next time.
      this._ocWslHigh = 0;
    }
    return dest;
  }

  // --- Xiaomi MiMo AI desktop ---------------------------------------------------
  // Assistant messages from the app's local API carry
  // tokens {input, output, reasoning, cache:{read,write}} where input EXCLUDES
  // cache (total = input+output+cacheRead+cacheWrite), so input is stored raw
  // and cache columns stay as detail only.
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
            input, // cache-exclusive: MiMo reports cache beside input
            output,
            reasoning: t.reasoning || 0,
            cacheRead, cacheWrite
          });
          added++;
        }
      }
      if (sig != null) { this._mimoSigs[s.id] = sig; this._sigsDirty = true; }
    }
    return added;
  }

  _mimoFetch(url, headers) {
    return fetch(url, { headers, signal: AbortSignal.timeout(5000) })
      .then((r) => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); });
  }

  // --- subscription plan credits -------------------------------------------------
  // Quota-based subscription plans don't emit per-message usage records; their
  // state is a snapshot (credits used out of a total). The user keeps that
  // snapshot in a JSON file and points the source at it; we re-read it on
  // every rescan (the file is tiny) and never let one bad file break the
  // token scan: unreadable/invalid plans are skipped, not fatal.
  _scanSubscription() {
    const src = this.cfg.sources.subscription;
    const before = this.subscriptionPlans;
    this.subscriptionPlans = null;
    if (!src || !src.enabled || !src.usagePath) return false;
    let parsed;
    try { parsed = JSON.parse(fs.readFileSync(src.usagePath, 'utf8')); } catch (_) { return false; }
    const plans = Array.isArray(parsed && parsed.plans) ? parsed.plans : [];
    const out = [];
    for (const p of plans) {
      if (!p || typeof p !== 'object') continue;
      const total = Number(p.total), used = Number(p.used);
      if (!Number.isFinite(total) || !Number.isFinite(used) || total <= 0) continue;
      out.push({
        name: String(p.name || 'Plan'),
        total,
        used: Math.max(0, used),
        resetsAt: (typeof p.resetsAt === 'string' && p.resetsAt) ? p.resetsAt : null
      });
    }
    this.subscriptionPlans = out.length ? out : null;
    return JSON.stringify(this.subscriptionPlans) !== JSON.stringify(before);
  }

  // --- cache ---------------------------------------------------------------------
  _loadCache() {
    try {
      if (!fs.existsSync(CACHE_PATH)) return;
      const parsed = JSON.parse(fs.readFileSync(CACHE_PATH, 'utf8'));
      // v4 = cache-EXCLUSIVE stored input (see the file header). Older caches
      // (bare arrays, v2/v3) hold cache-inclusive input values, so they are
      // discarded and the next scan rebuilds the records raw.
      const arr = (!Array.isArray(parsed) && parsed.v === 4 && Array.isArray(parsed.entries))
        ? parsed.entries : null;
      if (arr) for (const [k, r] of arr) this.records.set(k, r);
      else console.log('[wizbar] token cache: stale cache discarded; full rescan');
      // Restore the per-file byte cursors (see _readSessionTail) so a cold
      // start resumes from the last scan instead of re-reading the stores.
      if (arr && parsed.progress && typeof parsed.progress === 'object') {
        this._fileProgress = new Map();
        for (const [k, p] of Object.entries(parsed.progress)) {
          if (p && Number.isFinite(p.offset) && p.offset >= 0) {
            this._fileProgress.set(k, { offset: p.offset, mtimeMs: p.mtimeMs || 0 });
          }
        }
      }
      if (parsed.mimoSigs) this._mimoSigs = parsed.mimoSigs;
      if (arr) this._ocMtimeFloor = Math.max(0, ...arr.filter(([k]) => k.startsWith('o:')).map(([, r]) => r.ts || 0)) - 60000;
      if (arr) this._zaiMtimeFloor = Math.max(0, ...arr.filter(([k]) => k.startsWith('zf:')).map(([, r]) => r.ts || 0));
      if (arr) this._piMtimeFloor = Math.max(0, ...arr.filter(([k]) => k.startsWith('pf:')).map(([, r]) => r.ts || 0));
      console.log(`[wizbar] token cache: ${this.records.size} records`);
    } catch (_) {}
  }

  // Cache write is ASYNC and coalesced. It serializes up to 50k records; on
  // the scan cadence (every rescanMinutes with fresh records) a synchronous
  // write would hand the main process a periodic tens-of-ms stall, and the
  // bar's 60Hz follow loop lives in this process. tmp+rename keeps the file
  // atomically valid for the next cold start even if a write is interrupted.
  _saveCache() {
    if (this._saving) { this._savePending = true; return; } // one write in flight: rerun after it lands
    this._saving = true;
    try {
      const arr = [...this.records.entries()].slice(-50000);
      const progress = {};
      if (this._fileProgress) for (const [k, p] of this._fileProgress) progress[k] = p;
      const payload = JSON.stringify({ v: 4, entries: arr, mimoSigs: this._mimoSigs || {}, progress });
      // Unique tmp per write: two savers must never truncate each other's
      // staging file between write and rename.
      const tmp = `${CACHE_PATH}.${Date.now()}-${(this._tmpSeq = (this._tmpSeq || 0) + 1)}.tmp`;
      fs.writeFile(tmp, payload, 'utf8', (e) => {
        if (e) {
          this._saving = false;
          console.error('[wizbar] token cache write failed:', e.message);
          return;
        }
        fs.rename(tmp, CACHE_PATH, (e2) => {
          this._saving = false;
          if (e2) console.error('[wizbar] token cache swap failed:', e2.message);
          if (this._savePending) { this._savePending = false; this._saveCache(); }
        });
      });
    } catch (e) {
      this._saving = false;
      console.error('[wizbar] token cache write failed:', e.message);
    }
  }

  // Best-effort final write at quit: async saves may still be in flight when
  // the app closes, and the cache cursors are what keep the next cold start
  // from re-reading the whole store (a one-time multi-second scan).
  flushCacheSync() {
    if (!this.cfg.enabled || !this.records.size) return;
    try {
      const arr = [...this.records.entries()].slice(-50000);
      const progress = {};
      if (this._fileProgress) for (const [k, p] of this._fileProgress) progress[k] = p;
      fs.writeFileSync(CACHE_PATH, JSON.stringify({ v: 4, entries: arr, mimoSigs: this._mimoSigs || {}, progress }), 'utf8');
    } catch (e) {
      console.error('[wizbar] token cache flush failed:', e.message);
    }
  }

  // --- aggregation -----------------------------------------------------------------
  // Totals count input + output only, and both columns are cache-EXCLUSIVE
  // (see the file header). Cache read/write are detail columns that a table
  // may show beside the total; adding them would double the provider's own
  // reported numbers.
  aggregate() {
    if (!this.cfg.enabled) {
      // Master off: no sources are running, so the only honest dashboard state
      // is all zeros with nothing enabled.
      return {
        generatedAt: new Date().toISOString(), lastScan: null,
        today: { total: 0, apps: {} },
        week: 0, month: 0, allTime: 0,
        byDay: {}, byApp: {}, byModel: {},
        recordCount: 0, heatmapWeeks: this.cfg.heatmapWeeks, sourcesEnabled: 0, masterEnabled: false,
        subscription: null
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
      sourcesEnabled: Object.values(this.cfg.sources || {}).filter((s) => s && s.enabled).length,
      masterEnabled: true,
      // Subscription plan credits ride along as a sibling payload (null when
      // the source is off or nothing valid was read: the dashboard then hides
      // the card instead of showing an empty one).
      subscription: (this.cfg.enabled && this.subscriptionPlans)
        ? { plans: this.subscriptionPlans } : null
    };
  }
}

module.exports = { TokenTracker, localDateKey };
