'use strict';
// Regression check: case-variant model grouping on the token dashboard.
//
// Run: node scripts/model_case_regression.js
//
// The recorded model id casing is not stable — "GLM-5.3-Flash" and
// "glm-5.3-flash" appear in different sources and even inside one zai session
// file. aggregate() must merge them into ONE byModel group (counts summed),
// keep apps separate, and expose a single canonical display label
// (most frequent variant; ties break on codepoint order, so "GLM-5.3-Flash"
// wins an even split).
//
// Synthetic records only — never calls start()/rescan(), and clears the live
// token cache the constructor loads, so this is safe next to a running bar.

const path = require('path');
const os = require('os');
const fs = require('fs');
const { TokenTracker } = require('../src/tokens');

const cfg = { tokens: {
  rescanMinutes: 5,
  heatmapWeeks: 26,
  sources: {
    zcode: { enabled: false, dbPath: '' },
    zai: { enabled: false, sessionsDir: '' },
    opencode: { enabled: false, storageDir: '' }
  }
}};

function fail(msg) {
  console.error(`FAIL: ${msg}`);
  process.exit(1);
}

function rec(app, model, input, output) {
  return { app, ts: Date.now(), model, input, output, cacheRead: 0, cacheWrite: 0, reasoning: 0 };
}

function main() {
  const tracker = new TokenTracker(cfg);
  tracker.records.clear(); // drop the live token cache; this test is synthetic only

  tracker.records.set('a', rec('zai', 'GLM-5.3-Flash', 100, 10));
  tracker.records.set('b', rec('zai', 'GLM-5.3-Flash', 200, 20));
  tracker.records.set('c', rec('zai', 'glm-5.3-flash', 300, 30));
  tracker.records.set('d', rec('zcode', 'GLM-5.3-Flash', 400, 40));
  tracker.records.set('e', rec('zai', 'glm-5.3', 50, 5));
  tracker.records.set('f', rec('zai', 'GLM-5.3', 70, 7));
  tracker.records.set('g', rec('zai', 'xiaomi/mimo-x-flash-preview', 10, 1));
  tracker.records.set('h', rec('zai', null, 5, 0)); // missing model -> "unknown"

  const agg = tracker.aggregate();
  const keys = Object.keys(agg.byModel).sort();

  // Case variants merge (per app); apps stay separate; nothing else leaks in.
  const expectKeys = ['zai|glm-5.3', 'zai|glm-5.3-flash', 'zai|unknown', 'zai|xiaomi/mimo-x-flash-preview', 'zcode|glm-5.3-flash'];
  if (JSON.stringify(keys) !== JSON.stringify(expectKeys)) {
    fail(`byModel keys ${JSON.stringify(keys)} != ${JSON.stringify(expectKeys)}`);
  }

  const flash = agg.byModel['zai|glm-5.3-flash'];
  if (!flash) fail('zai flash group missing');
  if (flash.input !== 600 || flash.output !== 60 || flash.requests !== 3) {
    fail(`zai flash group not summed across case variants: ${JSON.stringify(flash)}`);
  }
  if (flash.modelLabel !== 'GLM-5.3-Flash') {
    fail(`zai flash label should be the majority variant GLM-5.3-Flash, got ${flash.modelLabel}`);
  }

  const zcodeFlash = agg.byModel['zcode|glm-5.3-flash'];
  if (zcodeFlash.input !== 400 || zcodeFlash.requests !== 1 || zcodeFlash.modelLabel !== 'GLM-5.3-Flash') {
    fail(`zcode flash group wrong (apps must not merge): ${JSON.stringify(zcodeFlash)}`);
  }

  // Even 1-1 split -> deterministic codepoint tie-break ("GLM-5.3" < "glm-5.3").
  const glm = agg.byModel['zai|glm-5.3'];
  if (glm.input !== 120 || glm.output !== 12 || glm.requests !== 2) {
    fail(`glm-5.3 group not summed: ${JSON.stringify(glm)}`);
  }
  if (glm.modelLabel !== 'GLM-5.3') fail(`glm-5.3 tie-break label should be GLM-5.3, got ${glm.modelLabel}`);

  // Untouched aggregations still count everything once.
  if (agg.byApp.zai.input !== 735 || agg.byApp.zai.requests !== 7) fail(`byApp.zai drifted: ${JSON.stringify(agg.byApp.zai)}`);
  if (agg.allTime !== 1248) fail(`allTime drifted: ${agg.allTime}`);

  // Every byModel group must carry exactly one display label.
  for (const [mk, group] of Object.entries(agg.byModel)) {
    if (typeof group.modelLabel !== 'string' || !group.modelLabel) fail(`group ${mk} has no modelLabel`);
  }

  console.log('PASS: case-variant models merge into one counted row with a canonical label; apps stay separate.');
}

// Live smoke against this machine's real stores (read-only; skipped when the
// zai sessions dir is absent). Re-sums the records independently and asserts
// every case-variant pair landed in exactly one counted group.
async function liveSmoke() {
  const home = os.homedir();
  const sessionsDir = path.join(home, '.zai', 'agent', 'sessions');
  if (!fs.existsSync(sessionsDir)) { console.log('skip: no zai sessions dir on this machine'); return; }
  const liveCfg = { tokens: {
    rescanMinutes: 5,
    heatmapWeeks: 26,
    sources: {
      zcode: { enabled: true, dbPath: path.join(home, '.zcode', 'cli', 'db', 'db.sqlite') },
      zai: { enabled: true, sessionsDir },
      opencode: { enabled: false, storageDir: '' }
    }
  }};
  const tracker = new TokenTracker(liveCfg);
  tracker.records.clear(); // drop the live token cache; measure this machine's scans only
  tracker._zaiMtimeFloor = 0; tracker._zaiMtimeHigh = 0;
  try { await tracker._scanZcode(); } catch (e) { console.log('note: zcode scan skipped:', e.message); }
  tracker._scanZaiSessions();
  const agg = tracker.aggregate();

  const expected = new Map(); // app|lowercased model -> { count, variants }
  for (const r of tracker.records.values()) {
    const mk = `${r.app}|${(r.model || 'unknown').toLowerCase()}`;
    const e = expected.get(mk) || { count: 0, variants: new Set() };
    e.count++; e.variants.add(r.model || 'unknown');
    expected.set(mk, e);
  }
  let mergedGroups = 0;
  for (const [mk, e] of expected) {
    const group = agg.byModel[mk];
    if (!group) fail(`live: group ${mk} missing from aggregate`);
    if (group.requests !== e.count) fail(`live: ${mk} counted ${group.requests} of ${e.count} records`);
    if (!e.variants.has(group.modelLabel)) fail(`live: ${mk} label ${group.modelLabel} not among its raw variants`);
    if (e.variants.size > 1) mergedGroups++;
  }
  const note = mergedGroups > 0
    ? `${mergedGroups} group(s) merged case variants from live data`
    : 'no case variants in live data (merge still exercised synthetically above)';
  console.log(`PASS: live stores — ${tracker.records.size} records, ${expected.size} model groups, ${note}.`);
}

Promise.resolve().then(main).then(liveSmoke).catch((e) => fail(e.stack || String(e)));
