'use strict';
// Regression for the usage estimator (tokens.estimateMissingUsage).
//
// Run: node scripts/estimate_regression.js
//
// Fully hermetic: it writes synthetic pi session files into a temp store and
// points a TokenTracker at them, so it never reads the real stores and never
// touches the live ~/.wizbar/token-cache.json. start() is never called, so no
// scan timer runs.
//
// Covers:
//   1. the estimator's own arithmetic (saturation, output scaling, empty input)
//   2. the per-file rule: a file with no real usage is a bridge session and its
//      blanks are estimated; a file with real usage keeps its blanks at zero,
//      because those are aborted turns and inventing tokens for them would
//      over-count
//   3. off-by-default, so the shipped behaviour is unchanged until configured
//   4. an appended read continues the running transcript size instead of
//      restarting it, which is what keeps a live session's estimates sane
//   5. an estimate never overwrites a real record

const fs = require('fs');
const os = require('os');
const path = require('path');
const assert = require('assert');

const FAKE_HOME = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-est-home-'));
process.env.HOME = FAKE_HOME;
process.env.USERPROFILE = FAKE_HOME;

const { TokenTracker } = require('../src/tokens');
const { estimateUsage, DEFAULTS } = require('../src/usage-estimate');

let failures = 0;
async function check(name, fn) {
  try { await fn(); console.log(`PASS: ${name}`); }
  catch (e) { failures++; console.log(`FAIL: ${name}\n      ${e.message}`); }
}

const STORE = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-est-store-'));
const T = Date.UTC(2026, 9, 3, 12, 0, 0);

function line(o) { return JSON.stringify(o) + '\n'; }
function user(id, text) { return line({ type: 'message', message: { id, role: 'user', content: text, timestamp: T } }); }
function asst(id, usage, text, model, provider) {
  const m = { id, role: 'assistant', content: text, timestamp: T, model: model || 'm' };
  if (provider) m.provider = provider;   // the route the turn travelled, which is what the rule keys on
  if (usage) m.usage = usage;
  return line({ type: 'message', message: m });
}
const ZERO = { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 };
const REAL = { input: 1000, output: 200, cacheRead: 5000, cacheWrite: 0 };
function write(name, rows) { fs.writeFileSync(path.join(STORE, name), rows.join('')); }

function cfg(over) {
  return { tokens: Object.assign({
    enabled: true,
    rescanMinutes: 5,
    heatmapWeeks: 26,
    estimateMissingUsage: false,
    estimate: Object.assign({}, DEFAULTS),
    labels: {},
    sources: { pi: { enabled: true, sessionsDir: STORE }, zai: { enabled: false, sessionsDir: '' },
               zcode: { enabled: false, dbPath: '' }, opencode: { enabled: false, storageDir: '' },
               mimo: { enabled: false }, subscription: { enabled: false, usagePath: '' } }
  }, over || {}) };
}

async function scan(c) {
  const t = new TokenTracker(c);
  await t._scanPiAgentSessions();
  return t;
}

// --- 1. the estimator's arithmetic ------------------------------------------
check('prompt saturates instead of running away on a long transcript', () => {
  const small = estimateUsage(400000, 1000, DEFAULTS);
  const huge = estimateUsage(40000000, 1000, DEFAULTS);
  assert.strictEqual(small.input, 84000);            // 400000/4 * 0.84
  assert.strictEqual(huge.input, DEFAULTS.saturateTokens);
  assert.ok(huge.input < 185001, 'saturation must cap the prompt');
});

check('output scales the reply text with no saturation', () => {
  const e = estimateUsage(10000, 4000, DEFAULTS);
  assert.strictEqual(e.output, Math.round(4000 / 4 * 1.019));
  assert.ok(e.output < 2000, 'output is the reply alone, never the transcript');
});

check('nothing to go on returns null rather than a fabricated zero', () => {
  assert.strictEqual(estimateUsage(0, 0, DEFAULTS), null);
  assert.strictEqual(estimateUsage(-5, -5, DEFAULTS), null);
});

check('config overrides win over the defaults', () => {
  const e = estimateUsage(100000, 1000, { inputFactor: 1, saturateTokens: 10, outputFactor: 1 });
  assert.strictEqual(e.input, 10);
});

// --- 2. the per-provider rule ----------------------------------------------
write('bridge.jsonl', [
  user('u1', 'do the thing'),
  asst('a1', ZERO, 'working on it', 'codebuddy-deepseek', 'bridge-a'),
  asst('a2', ZERO, 'still working', 'codebuddy-deepseek', 'bridge-a'),
  user('u2', 'and then'),
  asst('a3', ZERO, 'done', 'codebuddy-deepseek', 'bridge-a')
]);

write('mixed.jsonl', [
  user('u1', 'first'),
  asst('a1', ZERO, 'this turn was aborted', 'gpt', 'api-a'),   // api-a reports later: an abort
  asst('a2', REAL, 'a real answer', 'gpt', 'api-a'),
  user('u2', 'next'),
  asst('a3', ZERO, 'bridged turn in the same file', 'codebuddy2-deepseek', 'bridge-b'),
  asst('a4', REAL, 'another real answer', 'gpt', 'api-a')
]);

write('real.jsonl', [
  user('u1', 'hello'),
  asst('a1', REAL, 'real reply', 'gpt', 'api-a')
]);

check('a file with no real usage is treated as a bridge session and estimated', async () => {
  const t = await scan(cfg({ estimateMissingUsage: true }));
  const est = [...t.records.values()].filter((r) => r.app === 'pi' && r.estimated === true);
  const bridged = est.filter((r) => r.model === 'codebuddy-deepseek');
  assert.strictEqual(bridged.length, 3, `expected 3 estimated rows, got ${bridged.length}`);
  const total = bridged.reduce((a, r) => a + r.input + r.output + r.cacheRead + r.cacheWrite, 0);
  assert.ok(total > 0, 'the estimate must be non-zero');
});

check('a file that has real usage keeps its aborted blanks at zero', async () => {
  const t = await scan(cfg({ estimateMissingUsage: true }));
  const rows = [...t.records.values()].filter((r) => r.app === 'pi');
  // api-a reports, so its one blank is an abort and stays gone.
  const aborts = rows.filter((r) => r.estimated !== true);
  assert.strictEqual(aborts.length, 3, `expected only real turns, got ${aborts.length}`);
  const realTotal = aborts.reduce((a, r) => a + r.input + r.output + r.cacheRead + r.cacheWrite, 0);
  assert.strictEqual(realTotal, 3 * (1000 + 200 + 5000), 'real records must be untouched');
});

check('a bridged route inside a mixed file is still estimated', async () => {
  // the whole point of keying on the provider: 824 of 1,028 real CodeBuddy
  // turns live beside reporting routes in the same session file
  const t = await scan(cfg({ estimateMissingUsage: true }));
  const est = [...t.records.values()].filter((r) => r.app === 'pi' && r.estimated === true);
  const bridged = est.filter((r) => r.model === 'codebuddy2-deepseek');
  assert.strictEqual(bridged.length, 1, `expected the one bridged turn, got ${bridged.length}`);
  // and the abort on the reporting route is still left at zero
  assert.strictEqual(est.filter((r) => r.model === 'gpt').length, 0, 'an abort must not be estimated');
});

check('a real record is never replaced by an estimate', async () => {
  const t = await scan(cfg({ estimateMissingUsage: true }));
  const real = [...t.records.values()].find((r) => r.input === 1000);
  assert.ok(real, 'the real record must survive');
  assert.strictEqual(real.estimated, undefined);
});

// --- 3. off by default ------------------------------------------------------
check('with the flag off, blanks are dropped exactly as before', async () => {
  const t = await scan(cfg());
  const rows = [...t.records.values()].filter((r) => r.app === 'pi');
  assert.strictEqual(rows.length, 3, 'only the real turns, no estimates');
  assert.strictEqual(t.aggregate().estimatedRecords, 0);
});

check('aggregate() reports how many rows were estimated', async () => {
  const t = await scan(cfg({ estimateMissingUsage: true }));
  assert.strictEqual(t.aggregate().estimatedRecords, 4);
});

// --- 4. an appended read continues the running total ------------------------
check('appending to a session continues the transcript size instead of restarting it', async () => {
  const c = cfg({ estimateMissingUsage: true });
  const t = new TokenTracker(c);
  await t._scanPiAgentSessions();
  const est = () => [...t.records.values()].filter((r) => r.app === 'pi' && r.estimated === true && r.model === 'codebuddy-deepseek');
  assert.strictEqual(est().length, 3);

  // append one more blank turn to the bridge session
  const p = path.join(STORE, 'bridge.jsonl');
  fs.appendFileSync(p, asst('a4', ZERO, 'an extra turn after the append', 'codebuddy-deepseek', 'bridge-a'));
  fs.utimesSync(p, new Date(), new Date(Date.now() + 2000));
  await t._scanPiAgentSessions();
  assert.strictEqual(est().length, 4, 'the appended turn must be picked up exactly once');
  // if the char count had restarted at zero, the appended turn's prompt would
  // be far smaller than the first turn's, so the ordering is the assertion
  const inputs = est().map((r) => r.input).sort((a, b) => a - b);
  assert.ok(inputs[3] > inputs[0], `later turns must estimate larger prompts (got ${inputs})`);
});

// --- 5. the config defaults -------------------------------------------------
check('estimateMissingUsage defaults to false in the shipped config', () => {
  const src = fs.readFileSync(path.join(__dirname, '..', 'src', 'config.js'), 'utf8');
  assert.ok(/estimateMissingUsage:\s*false/.test(src),
    'the default must be off so a shipped install changes nothing');
});

(async () => {
  // every check above registered synchronously; let their promises settle first
  await new Promise((r) => setTimeout(r, 300));
  console.log(failures ? `\n${failures} FAILURE(S)` : '\nall estimator checks passed');
  process.exit(failures ? 1 : 0);
})();
