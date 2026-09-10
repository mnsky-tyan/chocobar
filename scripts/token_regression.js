'use strict';
// Regression check for the token pipeline's zai attribution.
//
// Run: node scripts/token_regression.js
//
// Exercises TokenTracker directly against the real stores on this machine
// (read-only; never calls start(), so the live token cache is not touched):
//   before = zcode DB scan only (the pre-2026-09-10 reader, which missed the
//            rebuilt pi engine's usage entirely)
//   after  = + pi session-file scan (src/tokens.js _scanZaiSessions)
// It then re-sums the raw pi files independently and asserts the scanner's
// per-day delta matches the raw data exactly, and that post-rebuild days go
// from nothing to non-zero.

const fs = require('fs');
const os = require('os');
const path = require('path');
const { TokenTracker, localDateKey } = require('../src/tokens');

const DAY_MS = 86400000;
const DAYS_SHOWN = 6;

const home = os.homedir();
const cfg = { tokens: {
  rescanMinutes: 5,
  heatmapWeeks: 26,
  sources: {
    zcode: { enabled: true, dbPath: path.join(home, '.zcode', 'cli', 'db', 'db.sqlite') },
    zai: { enabled: true, sessionsDir: path.join(home, '.zai', 'agent', 'sessions') },
    opencode: { enabled: false, storageDir: '' }
  }
}};

function fail(msg) {
  console.error(`FAIL: ${msg}`);
  process.exit(1);
}

// Independent raw sum over new-format pi session files (no tracker code involved).
function rawZaiByDay(sessionsDir) {
  const byDay = new Map();
  let files = [];
  try { files = fs.readdirSync(sessionsDir); } catch (_) {}
  for (const fn of files) {
    if (!fn.endsWith('.jsonl') || fn.startsWith('ZCODE_')) continue;
    let lines;
    try { lines = fs.readFileSync(path.join(sessionsDir, fn), 'utf8').split('\n'); } catch (_) { continue; }
    for (const line of lines) {
      if (!line.includes('"usage"')) continue;
      let d;
      try { d = JSON.parse(line); } catch (_) { continue; }
      if (d.type !== 'message' || !d.message || d.message.role !== 'assistant') continue;
      const u = d.message.usage || {};
      const input = u.input || 0, output = u.output || 0;
      if (!(input || output || u.cacheRead || u.cacheWrite)) continue;
      const dk = localDateKey(Number(d.message.timestamp));
      byDay.set(dk, (byDay.get(dk) || 0) + input + output);
    }
  }
  return byDay;
}

function zaiByDayFromRecords(records) {
  const byDay = new Map();
  for (const r of records.values()) {
    if (r.app !== 'zai') continue;
    const dk = localDateKey(r.ts);
    byDay.set(dk, (byDay.get(dk) || 0) + (r.input || 0) + (r.output || 0));
  }
  return byDay;
}

function main() {
  if (!fs.existsSync(cfg.tokens.sources.zcode.dbPath)) fail(`zcode DB missing: ${cfg.tokens.sources.zcode.dbPath}`);
  if (!fs.existsSync(cfg.tokens.sources.zai.sessionsDir)) fail(`zai sessions dir missing: ${cfg.tokens.sources.zai.sessionsDir}`);

  const raw = rawZaiByDay(cfg.tokens.sources.zai.sessionsDir);
  const rawTotal = [...raw.values()].reduce((a, b) => a + b, 0);
  if (!rawTotal) fail('no new-format zai session files with usage found — regression precondition not met');

  const tracker = new TokenTracker(cfg);
  tracker.records.clear(); // drop the live token cache; measure this machine's scans only

  tracker._scanZcode();
  const before = zaiByDayFromRecords(tracker.records);

  tracker._scanZaiSessions();
  const after = zaiByDayFromRecords(tracker.records);

  const days = [];
  for (let i = DAYS_SHOWN - 1; i >= 0; i--) days.push(localDateKey(Date.now() - i * DAY_MS));

  console.log('zai in+out per local day  (before = DB only, after = DB + pi session files):');
  console.log('  day         before          after  pi-files');
  for (const dk of days) {
    console.log(`  ${dk}  ${(before.get(dk) || 0).toLocaleString().padStart(12)}  ${(after.get(dk) || 0).toLocaleString().padStart(12)}  ${(raw.get(dk) || 0).toLocaleString().padStart(12)}`);
  }

  for (const dk of days) {
    const delta = (after.get(dk) || 0) - (before.get(dk) || 0);
    const expect = raw.get(dk) || 0;
    if (delta !== expect) fail(`day ${dk}: scanner added ${delta} but raw pi files hold ${expect}`);
  }

  const recentRaw = (raw.get(days[days.length - 1]) || 0) + (raw.get(days[days.length - 2]) || 0);
  if (!recentRaw) fail('no pi-file usage in the last two days — scanner or store may be stale');
  if ((after.get(days[days.length - 1]) || 0) + (after.get(days[days.length - 2]) || 0) < recentRaw) {
    fail('post-rebuild zai usage not recovered by the scan');
  }

  console.log(`PASS: pi-file scan recovers ${(recentRaw).toLocaleString()} zai tokens from the last 2 days; per-day deltas match raw data exactly.`);
}

main();
