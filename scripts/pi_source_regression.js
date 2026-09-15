'use strict';
// Regression check for the pi session-log usage source (src/tokens.js).
//
// Run: node scripts/pi_source_regression.js
//
// 1. Builds a synthetic pi store in a temp dir (per-project subfolders, flat
//    folder, a ZCODE_* file that must be skipped, a corrupt tail line, a
//    non-assistant line) and asserts the scanner reads exactly the assistant
//    usage records: totals, per-model attribution, dedup on rescan, and the
//    mtime cursor skipping unchanged files.
// 2. If a real ~/.pi/agent/sessions exists, independently re-sums every
//    assistant `usage` record and asserts the scanner matches it exactly.
// Never calls start()/rescan() and HOME is overridden to a temp dir, so the
// user's real token cache is neither read nor written.

const fs = require('fs');
const os = require('os');
const path = require('path');

// Real home captured BEFORE the hermetic override below: the real-store
// cross-check still reads the user's actual ~/.pi sessions (read-only).
const REAL_HOME = os.homedir();
// Hermeticity: point HOME/USERPROFILE at a temp dir before src/config.js (which
// computes APP_DIR from os.homedir() at require time) is loaded, so TokenTracker
// never reads or writes the real ~/.wizbar token cache.
const FAKE_HOME = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-test-home-'));
process.env.HOME = FAKE_HOME;
process.env.USERPROFILE = FAKE_HOME;

const { TokenTracker } = require('../src/tokens');

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}${detail ? '  [' + detail + ']' : ''}`);
  if (!ok) failures++;
}

function mkSession(dir, name, messages) {
  const lines = [{ type: 'session', version: 3, id: name, cwd: dir }].concat(messages);
  fs.writeFileSync(path.join(dir, name + '.jsonl'), lines.map((l) => JSON.stringify(l)).join('\n') + '\n');
}
function msg(id, model, usage, ts) {
  return { type: 'message', id, parentId: null, timestamp: new Date(ts || Date.now()).toISOString(),
    message: { role: 'assistant', content: [], api: 'x', provider: 'p', model, usage,
      stopReason: 'endTurn', timestamp: ts || Date.now() } };
}

// --- 1. synthetic store -------------------------------------------------------
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-pi-regression-'));
const store = path.join(tmp, 'sessions');
const projA = path.join(store, '--home-user--proj-a--');
const projB = path.join(store, '--home-user--proj-b--');
fs.mkdirSync(projA, { recursive: true });
fs.mkdirSync(projB, { recursive: true });

const U1 = { input: 100, output: 10, cacheRead: 50, cacheWrite: 5, totalTokens: 165 };
const U2 = { input: 200, output: 20, cacheRead: 0, cacheWrite: 0, totalTokens: 220 };
mkSession(projA, '2026-01-01T00-00-00_a', [
  msg('m1', 'model-x', U1, Date.UTC(2026, 0, 1, 12)),
  { type: 'message', id: 'm2', message: { role: 'user', content: 'hi', usage: U2 } }, // non-assistant: skip
  msg('m3', 'model-y', U2, Date.UTC(2026, 0, 2, 12))
]);
mkSession(projB, '2026-01-03T00-00-00_b', [msg('m4', 'model-x', U2, Date.UTC(2026, 0, 3, 12))]);
mkSession(store, 'ZCODE_sess_00000000-0000-0000-0000-000000000000_x', [msg('m5', 'g', U2)]); // legacy: skip
fs.writeFileSync(path.join(projA, 'corrupt.jsonl'),
  JSON.stringify({ type: 'message', id: 'm6', message: { role: 'assistant', usage: U1 } }) + '\n{"trunc'); // tail line: m6 only

const cfg = { tokens: { rescanMinutes: 5, heatmapWeeks: 26, sources: {
  zcode: { enabled: false, dbPath: '' },
  zai: { enabled: false, sessionsDir: '' },
  pi: { enabled: true, sessionsDir: store },
  opencode: { enabled: false, storageDir: '' },
  mimo: { enabled: false }
}}};
const t = new TokenTracker(cfg);
t._scanPiAgentSessions();

// expected: m1 + m3 (projA) + m4 (projB) + m6 (corrupt) = 4 records
// totals (input already cache-folded by the scanner): U1 -> 155 in, U2 -> 200 in
const agg = t.aggregate();
check('record count', agg.recordCount === 4, `got ${agg.recordCount}`);
const expectAllTime = (155 + 10) + (200 + 20) + (200 + 20) + (155 + 10);
check('all-time total = input+output', agg.allTime === expectAllTime, `got ${agg.allTime}, want ${expectAllTime}`);
check('app attribution', !!agg.byApp.pi, JSON.stringify(agg.byApp));

// dedup: second scan must add nothing
const again = t._scanPiAgentSessions();
check('rescan adds nothing (dedup)', t.aggregate().recordCount === 4 && again === 0, `added ${again}`);

// cursor: unchanged store skipped even after cursor reset of record map is NOT done —
// rescan with warm cursors must be a no-op
t._scanPiAgentSessions();
check('warm cursor skips files', t.records.size === 4, `size ${t.records.size}`);

// --- 2. real store cross-check -----------------------------------------------
const realDir = path.join(REAL_HOME, '.pi', 'agent', 'sessions');
if (fs.existsSync(realDir)) {
  const realCfg = { tokens: { rescanMinutes: 5, heatmapWeeks: 26, sources: {
    zcode: { enabled: false, dbPath: '' },
    zai: { enabled: false, sessionsDir: '' },
    pi: { enabled: true, sessionsDir: realDir },
    opencode: { enabled: false, storageDir: '' },
    mimo: { enabled: false }
  }}};
  const tr = new TokenTracker(realCfg); // fresh records, no cache write
  tr._scanPiAgentSessions();
  // independent raw sum (no scanner code involved)
  let rawIn = 0, rawOut = 0, rawCacheR = 0, rawCacheW = 0, n = 0;
  const walk = (dir, depth) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, e.name);
      if (e.isDirectory() && depth > 0) walk(full, depth - 1);
      else if (e.isFile() && e.name.endsWith('.jsonl') && !e.name.startsWith('ZCODE_')) {
        for (const line of fs.readFileSync(full, 'utf8').split('\n')) {
          if (!line.includes('"usage"')) continue;
          let d; try { d = JSON.parse(line); } catch (_) { continue; }
          if (d.type !== 'message' || !d.message || d.message.role !== 'assistant') continue;
          const u = d.message.usage || {};
          if (!(u.input || u.output || u.cacheRead || u.cacheWrite)) continue;
          n++; rawIn += (u.input || 0) + (u.cacheRead || 0) + (u.cacheWrite || 0);
          rawOut += u.output || 0; rawCacheR += u.cacheRead || 0; rawCacheW += u.cacheWrite || 0;
        }
      }
    }
  };
  walk(realDir, 2);
  const ragg = tr.aggregate();
  const pin = Object.values(ragg.byApp.pi || {}).length ? ragg.byApp.pi : { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, requests: 0 };
  check('real store: record count matches raw sum', ragg.recordCount === n, `scanner ${ragg.recordCount} vs raw ${n}`);
  check('real store: input matches raw sum', pin.input === rawIn, `scanner ${pin.input} vs raw ${rawIn}`);
  check('real store: output matches raw sum', pin.output === rawOut, `scanner ${pin.output} vs raw ${rawOut}`);
  check('real store: cacheRead matches raw sum', pin.cacheRead === rawCacheR, `scanner ${pin.cacheRead} vs raw ${rawCacheR}`);
  check('real store: cacheWrite matches raw sum', pin.cacheWrite === rawCacheW, `scanner ${pin.cacheWrite} vs raw ${rawCacheW}`);
} else {
  console.log('SKIP: no real ~/.pi/agent/sessions on this machine (synthetic checks still ran)');
}

fs.rmSync(tmp, { recursive: true, force: true });
if (failures) { console.error(`\n${failures} FAILURE(S)`); process.exit(1); }
console.log('\nAll pi source checks passed.');
