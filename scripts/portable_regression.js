'use strict';
// Cross-platform regression for WizBar's portability layer + perf-critical
// pure logic. Runs on Windows AND Linux/macOS with no GUI and no Electron:
//
//   node scripts/portable_regression.js
//
// Covers: sysfs battery/CPU-temp readers (synthetic trees), metrics dirty
// tracking (the on-change stats push), config defaults sanity, koffi-less
// module load (child process), the tokens aggregate empty state, and the
// static-pill geometry handshake (BarWindow against a stubbed window).

const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFile } = require('child_process');

// Hermeticity: os.homedir() feeds APP_DIR (~/.wizbar) in src/config.js, which
// TokenTracker's constructor reads the token cache from. Point HOME/USERPROFILE
// at a temp dir BEFORE any src module is required so the tests can never touch
// the real ~/.wizbar on any platform.
const FAKE_HOME = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-test-home-'));
process.env.HOME = FAKE_HOME;
process.env.USERPROFILE = FAKE_HOME;

let failures = 0;
let pendingAsync = 1; // the section-6 child-process callback
function asyncFinished() {
  if (--pendingAsync === 0) done();
}
function check(name, ok, detail) {
  console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}${detail ? '  [' + detail + ']' : ''}`);
  if (!ok) failures++;
}
const W = (f, s) => { fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, s); };

// --- 1. Linux battery reader (synthetic /sys tree) ----------------------------
const native = require('../src/native');
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-bat-'));
  W(path.join(dir, 'BAT0/type'), 'Battery\n');
  W(path.join(dir, 'BAT0/capacity'), '84\n');
  W(path.join(dir, 'BAT0/status'), 'Discharging\n');
  W(path.join(dir, 'AC/type'), 'Mains\n');
  W(path.join(dir, 'AC/online'), '1\n');
  const b = native.getBatteryLinux(dir);
  check('battery: capacity/status/AC', b && b.percent === 84 && b.ac === true && b.charging === false,
    JSON.stringify(b));
  W(path.join(dir, 'BAT0/status'), 'Charging\n');
  const b2 = native.getBatteryLinux(dir);
  check('battery: charging flag', b2 && b2.charging === true, JSON.stringify(b2));

  const noBat = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-bat2-'));
  W(path.join(noBat, 'AC/type'), 'Mains\n');
  W(path.join(noBat, 'AC/online'), '1\n');
  const b3 = native.getBatteryLinux(noBat);
  check('battery: desktop without battery', b3 && b3.noBattery === true && b3.percent === null,
    JSON.stringify(b3));
}

// --- 2. Linux CPU temperature reader (synthetic hwmon + thermal zones) --------
{
  const hw = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-hw-'));
  const tz = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-tz-'));
  W(path.join(hw, 'coretemp/temp1_input'), '45000\n');
  W(path.join(hw, 'coretemp/temp1_label'), 'Package id 0\n');
  W(path.join(hw, 'coretemp/temp2_input'), '41000\n');
  W(path.join(hw, 'coretemp/temp2_label'), 'Core 0\n');
  const t = native.getCpuTempLinux(hw, tz);
  check('temp: hwmon Package preferred', t.state === 'ok' && t.c === 45 && /package/i.test(t.label),
    JSON.stringify(t));

  const hw2 = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-hw2-'));
  const tz2 = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-tz2-'));
  W(path.join(hw2, 'acpitz/temp1_input'), '999000\n'); // insane value: skipped
  W(path.join(tz2, 'thermal_zone0/type'), 'acpitz\n');
  W(path.join(tz2, 'thermal_zone0/temp'), '39000\n');
  W(path.join(tz2, 'thermal_zone1/type'), 'x86_pkg_temp\n');
  W(path.join(tz2, 'thermal_zone1/temp'), '52000\n');
  const t2 = native.getCpuTempLinux(hw2, tz2);
  check('temp: thermal_zone fallback picks x86_pkg_temp', t2.state === 'ok' && t2.c === 52,
    JSON.stringify(t2));

  const empty = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-hw3-'));
  const t3 = native.getCpuTempLinux(empty, empty);
  check('temp: nothing readable -> no-section', t3.state === 'no-section', JSON.stringify(t3));
}

// --- 3. metrics dirty tracking (on-change stats push) -------------------------
{
  const { MetricsEngine } = require('../src/metrics');
  const { DEFAULTS } = require('../src/config');
  const m = new MetricsEngine(DEFAULTS);
  check('dirty: starts clean', m.consumeDirty() === false && m._dirty === false);
  m._pollRam();
  const first = m.consumeDirty(); // first poll: null -> value = change
  m._pollRam();                   // same value again: no change
  check('dirty: same-value poll is clean', first === true && m._dirty === false && m.consumeDirty() === false);

  // drive _pollCpuTemp through a patched native reader
  const real = native.getHwinfoTemp;
  native.getHwinfoTemp = () => ({ state: 'ok', c: 50, label: 'Package id 0' });
  m._pollCpuTemp();
  const changed = m.consumeDirty();
  native.getHwinfoTemp = () => ({ state: 'ok', c: 50, label: 'Package id 0' });
  m._pollCpuTemp();
  const same = m._dirty;
  native.getHwinfoTemp = () => ({ state: 'ok', c: 65, label: 'Package id 0' });
  m._pollCpuTemp();
  const changed2 = m.consumeDirty();
  native.getHwinfoTemp = real;
  check('dirty: temp change/dedupe/change', changed === true && same === false && changed2 === true,
    `${changed}/${same}/${changed2}`);
  check('snapshot has no now field', !('now' in m.snapshot()));
}

// --- 4. config defaults sanity (public-release neutrality) --------------------
{
  const { DEFAULTS } = require('../src/config');
  check('config: token sources off, no paths',
    DEFAULTS.tokens.enabled === false &&
    Object.values(DEFAULTS.tokens.sources).every((s) => !s.enabled) &&
    Object.values(DEFAULTS.tokens.sources).every((s) => !s.dbPath && !s.sessionsDir && !s.storageDir));
  check('config: pet off, empty path; agents module fully removed',
    DEFAULTS.modules.pet.enabled === false && DEFAULTS.modules.pet.exePath === '' &&
    !('agents' in DEFAULTS.modules));
  const blob = JSON.stringify(DEFAULTS);
  check('config: no personal identifiers in defaults',
    !/tyanw|tyan|mnsky|firstmate/i.test(blob));
}

// --- 5. tokens aggregate empty state + master switch --------------------------
{
  const { TokenTracker } = require('../src/tokens');
  const { DEFAULTS } = require('../src/config');
  const t = new TokenTracker({ ...DEFAULTS, tokens: DEFAULTS.tokens });
  const agg = t.aggregate();
  check('tokens: empty state is zeros + sourcesEnabled 0',
    agg.recordCount === 0 && agg.today.total === 0 && agg.sourcesEnabled === 0);

  // Master switch: enabled=false means ZERO scans even when a source is on;
  // the same store must scan once the master is switched on.
  const store = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-pi-master-'));
  fs.mkdirSync(path.join(FAKE_HOME, '.wizbar'), { recursive: true }); // cache dir, as the app creates it
  const proj = path.join(store, '--home-user--proj--');
  fs.mkdirSync(proj, { recursive: true });
  const msg = JSON.stringify({ type: 'message', id: 'm1', message: {
    role: 'assistant', model: 'model-x', timestamp: Date.UTC(2026, 0, 1, 12),
    usage: { input: 100, output: 10, cacheRead: 0, cacheWrite: 0 } } });
  fs.writeFileSync(path.join(proj, 's1.jsonl'), msg + '\n');
  const mkCfg = (enabled) => ({ ...DEFAULTS, tokens: {
    ...DEFAULTS.tokens, enabled, rescanMinutes: 5,
    sources: { ...DEFAULTS.tokens.sources, pi: { enabled: true, sessionsDir: store } }
  } });
  pendingAsync++;
  (async () => {
    const off = await new TokenTracker(mkCfg(false)).rescan();
    check('tokens: master off with a source on scans nothing',
      off.recordCount === 0 && off.today.total === 0 && off.sourcesEnabled === 0,
      JSON.stringify({ recordCount: off.recordCount, sourcesEnabled: off.sourcesEnabled }));
    const on = await new TokenTracker(mkCfg(true)).rescan();
    check('tokens: master on scans the enabled source',
      on.recordCount === 1 && on.allTime === 110 && on.sourcesEnabled === 1,
      JSON.stringify({ recordCount: on.recordCount, allTime: on.allTime }));
  })().catch((e) => check('tokens: master-switch block', false, e.message))
    .finally(asyncFinished);
}

// --- 6. koffi-less module load (child process; broken native must not crash) --
{
  const child = path.join(os.tmpdir(), 'wizbar-koffiless-' + Date.now() + '.js');
  fs.writeFileSync(child, `
    const Module = require('module');
    const orig = Module.prototype.require;
    Module.prototype.require = function (id) {
      if (id === 'koffi') throw new Error('simulated koffi failure');
      return orig.apply(this, arguments);
    };
    const n = require('${path.join(__dirname, '..', 'src', 'native').replace(/\\/g, '\\\\')}');
    const ok = n.getBattery() !== undefined && Array.isArray(n.listWindowsByClass('X'));
    console.log(ok ? 'KOFFILESS_OK' : 'KOFFILESS_BAD');
  `);
  const out = execFile(process.execPath, [child], { encoding: 'utf8', timeout: 20000 },
    (err, stdout) => {
      check('native: loads with koffi completely broken', !err && /KOFFILESS_OK/.test(stdout),
        err ? err.message : (stdout || '').trim());
      fs.rmSync(child, { force: true });
      asyncFinished();
    });
}
// --- 7. battery charging semantics (100% on AC renders green) -----------------
{
  const cases = [
    { name: 'win: 100% on AC (flag dropped at top-of-charge) is charging',
      sps: { ACLineStatus: 1, BatteryFlag: 0x1, BatteryLifePercent: 100 }, want: true },
    { name: 'win: actively charging below 100 is charging',
      sps: { ACLineStatus: 1, BatteryFlag: 0x8, BatteryLifePercent: 42 }, want: true },
    { name: 'win: charging flag stuck set after unplug is NOT charging',
      sps: { ACLineStatus: 0, BatteryFlag: 0x8, BatteryLifePercent: 60 }, want: false },
    { name: 'win: on AC mid-charge without flag is not charging',
      sps: { ACLineStatus: 1, BatteryFlag: 0x1, BatteryLifePercent: 55 }, want: false },
    { name: 'win: 99% holding on AC (trickle, flag dropped) not charging',
      sps: { ACLineStatus: 1, BatteryFlag: 0x1, BatteryLifePercent: 99 }, want: false }
  ];
  for (const c of cases) {
    const b = native.batteryFromPowerStatus(c.sps);
    check(c.name, b.charging === c.want, JSON.stringify(b));
  }
  // Linux reader: status "Full" + AC online = plugged at 100% -> charging green
  const full = fs.mkdtempSync(path.join(os.tmpdir(), 'wizbar-batfull-'));
  W(path.join(full, 'BAT0/type'), 'Battery\n');
  W(path.join(full, 'BAT0/capacity'), '100\n');
  W(path.join(full, 'BAT0/status'), 'Full\n');
  W(path.join(full, 'AC/type'), 'Mains\n');
  W(path.join(full, 'AC/online'), '1\n');
  const bf = native.getBatteryLinux(full);
  check('linux: 100% Full on AC is charging', bf.percent === 100 && bf.charging === true, JSON.stringify(bf));
}

// --- 8. static-pill geometry handshake (BarWindow against a stubbed window) ---
{
  const Module = require('module');
  const origLoad = Module._load;
  class StubWin {
    constructor(opts) {
      this.opts = opts; this.bounds = null; this.destroyed = false; this.visible = false;
    }
    isDestroyed() { return this.destroyed; }
    isVisible() { return this.visible; }
    setBounds(b) { this.bounds = b; }
    getContentSize() { return [this.bounds ? this.bounds.width : 900, this.opts.height || 24]; }
    showInactive() { this.visible = true; }
    on() {} loadFile() {} setSkipTaskbar() {} setContentSize() {}
    getNativeWindowHandle() { return Buffer.alloc(8); }
  }
  Module._load = function (request) {
    if (request === 'electron') {
      return {
        BrowserWindow: StubWin,
        screen: { getPrimaryDisplay: () => ({ workArea: { x: 0, y: 0, width: 1920, height: 1040 } }) }
      };
    }
    return origLoad.apply(this, arguments);
  };
  const { BarWindow } = require('../src/bar');
  const cfg = { bar: { height: 24 } };
  const bar = new BarWindow(cfg);
  bar.create();
  const win = bar.win;
  const bounds = { x: 0, y: 0, width: 1920, height: 24, scale: 1 };

  bar.applyGeometry(bounds, 'content');
  bar.setPillWidth(320, cfg.bar);
  check('pill: shrinks to the renderer-reported width',
    win.bounds && win.bounds.width === 320, JSON.stringify(win.bounds));

  // Regression: a config save re-emits identical geometry; in content mode the
  // shrunk window IS the correct size and must not be re-expanded (the
  // full-width strip would swallow top-edge clicks until the next report).
  bar.applyGeometry(bounds, 'content');
  check('pill: unchanged bounds in content mode keep the pill size',
    win.bounds && win.bounds.width === 320, JSON.stringify(win.bounds));

  // Regression: leaving content mode must re-expand even with identical
  // bounds, or the full-strip layout stays truncated in a pill-sized window.
  bar.applyGeometry(bounds, 'workarea');
  check('pill: leaving content mode re-expands despite unchanged bounds',
    win.bounds && win.bounds.width === 1920, JSON.stringify(win.bounds));
  check('pill: reported width forgotten after re-expand', bar._lastPillW === 0);

  // Regression: content mode ENTERED with identical bounds must still be
  // registered as the current mode, or the return flip to 'workarea' would
  // leave the window truncated at the pill width forever.
  bar.applyGeometry(bounds, 'content');
  check('pill: entering content mode with identical bounds is a no-op',
    win.bounds && win.bounds.width === 1920, JSON.stringify(win.bounds));
  bar.setPillWidth(320, cfg.bar);
  check('pill: entered content mode shrinks via renderer report',
    win.bounds && win.bounds.width === 320, JSON.stringify(win.bounds));
  bar.applyGeometry(bounds, 'workarea');
  check('pill: workarea->content->workarea round trip re-expands',
    win.bounds && win.bounds.width === 1920, JSON.stringify(win.bounds));

  bar.setPillWidth(320, cfg.bar);
  const wider = { x: 0, y: 0, width: 2560, height: 24, scale: 1 };
  bar.applyGeometry(wider, 'content');
  check('pill: genuinely changed bounds re-apply full geometry',
    win.bounds && win.bounds.width === 2560, JSON.stringify(win.bounds));

  Module._load = origLoad;
}

// --- 9. subscription plan-usage source ---------------------------------------
{
  const { TokenTracker } = require('../src/tokens');
  const { DEFAULTS } = require('../src/config');
  fs.mkdirSync(path.join(FAKE_HOME, '.wizbar'), { recursive: true }); // cache dir, as the app creates it
  const usageFile = path.join(FAKE_HOME, 'plan-usage.json');
  fs.writeFileSync(usageFile, JSON.stringify({ plans: [
    { name: 'Pro Plan', total: 1500, used: 430, resetsAt: '2026-10-14' },
    { name: 'bad total', total: 0, used: 5 },        // skipped: non-positive total
    { name: 'no numbers', total: 'x', used: 'y' }    // skipped: non-numeric quota
  ] }));
  const mkCfg = (masterOn, subOn) => ({ ...DEFAULTS, tokens: {
    ...DEFAULTS.tokens, enabled: masterOn, rescanMinutes: 5,
    sources: { ...DEFAULTS.tokens.sources, subscription: { enabled: subOn, usagePath: usageFile } }
  } });
  check('subscription: default source is off with empty path',
    DEFAULTS.tokens.sources.subscription.enabled === false &&
    DEFAULTS.tokens.sources.subscription.usagePath === '');
  pendingAsync++;
  (async () => {
    const off = await new TokenTracker(mkCfg(true, false)).rescan();
    check('subscription: source off -> null payload, no plan card',
      off.subscription === null && off.sourcesEnabled === 0);
    const on = await new TokenTracker(mkCfg(true, true)).rescan();
    const plans = on.subscription && on.subscription.plans;
    check('subscription: enabled reads valid plans, skips invalid entries',
      on.sourcesEnabled === 1 && plans && plans.length === 1 &&
      plans[0].name === 'Pro Plan' && plans[0].total === 1500 &&
      plans[0].used === 430 && plans[0].resetsAt === '2026-10-14',
      JSON.stringify(on.subscription));
    const master = await new TokenTracker(mkCfg(false, true)).rescan();
    check('subscription: master off -> null payload even with the source on',
      master.subscription === null && master.masterEnabled === false);
    fs.writeFileSync(usageFile, '{ broken');
    const bad = await new TokenTracker(mkCfg(true, true)).rescan();
    check('subscription: malformed file -> null payload, scan survives', bad.subscription === null);
    fs.writeFileSync(usageFile, JSON.stringify({ plans: [] }));
    const empty = await new TokenTracker(mkCfg(true, true)).rescan();
    check('subscription: empty plans -> null payload', empty.subscription === null);
  })().catch((e) => check('subscription: block', false, e.message))
    .finally(asyncFinished);
}

// --- 10. terminal auto-probe resolution ---------------------------------------
{
  const Module = require('module');
  const origLoad = Module._load;
  Module._load = function (request) {
    if (request === 'electron') {
      return { screen: { on() {}, removeListener() {}, getPrimaryDisplay: () => ({ workArea: { x: 0, y: 0, width: 1920, height: 1040 } }) } };
    }
    return origLoad.apply(this, arguments);
  };
  const { TerminalTracker, resolveProbe } = require('../src/tracker');
  const auto = resolveProbe('');
  check('probe: auto = WT/conhost/ConEmu/mintty classes then process probes',
    JSON.stringify(auto.classes) === JSON.stringify(['CASCADIA_HOSTING_WINDOW_CLASS', 'ConsoleWindowClass', 'VirtualConsoleClass', 'mintty']) &&
    JSON.stringify(auto.processes) === JSON.stringify(['wezterm-gui.exe', 'alacritty.exe', 'Hyper.exe']),
    JSON.stringify(auto));
  const pinned = resolveProbe('Foo_CLASS');
  check('probe: string override pins one class',
    pinned.classes.length === 1 && pinned.classes[0] === 'Foo_CLASS' && pinned.processes.length === 0);
  const t = new TerminalTracker({ terminal: { className: '' }, bar: { height: 24 } });
  check('probe: default config resolves auto mode', t.probe.classes.length === 4 && t.probe.processes.length === 3);
  // Headless stubs: no windows and no processes anywhere -> no candidates, no crash.
  check('probe: empty environment yields no candidates', JSON.stringify(t._listCandidates()) === '[]');
  Module._load = origLoad;
}

// --- 10. subscription board tracker: bounded fetches + last-good retention ---
{
  const { SubsTracker } = require('../src/subs');
  const { DEFAULTS } = require('../src/config');

  check('subs: defaults ship everything off with sane bounds',
    DEFAULTS.subs.enabled === false && DEFAULTS.subs.fetchTimeoutMs === 20000 &&
    Array.isArray(DEFAULTS.subs.providers) && DEFAULTS.subs.providers.every((p) => p.enabled === false));

  const baseCfg = (over) => ({ ...DEFAULTS, subs: {
    enabled: true, intervalMinutes: 2, fetchTimeoutMs: 20000,
    providers: [{ type: 'chatgpt', enabled: true, label: 'Test', authPath: '/none' }],
    ...over
  } });

  // Fake fetch that hangs PAST any deadline but honors the abort signal, the
  // way a real stalled endpoint behaves.
  const hangingFetch = (u, o) => new Promise((resolve, reject) => {
    const t = setTimeout(() => resolve({ ok: true, json: async () => ({}) }), 30000);
    if (o && o.signal) o.signal.addEventListener('abort', () => { clearTimeout(t); reject(new Error('The operation was aborted')); });
  });

  // The chatgpt adapter reads its credential before fetching; give it a real
  // fixture so the boundedness test exercises the fetch path.
  const authFile = path.join(FAKE_HOME, 'codex-auth.json');
  fs.writeFileSync(authFile, JSON.stringify({ auth_mode: 'chatgpt', tokens: { access_token: 'test-token' } }));
  const chatgptProvider = (over) => ({ type: 'chatgpt', enabled: true, label: 'Test', authPath: authFile, ...over });

  pendingAsync++;
  (async () => {
    // Bounded: a provider override under 100ms keeps the whole rescan fast
    // even when the endpoint never answers.
    const t = new SubsTracker(baseCfg(), hangingFetch);
    t.cfg.providers = [chatgptProvider({ timeoutMs: 80 })]; // clamps to the 3s floor
    const t0 = Date.now();
    const snap = await t.rescan();
    const took = Date.now() - t0;
    const p = snap.providers['chatgpt:0'];
    check('subs: hung endpoint cannot stall a rescan (bounded by deadline)',
      took < 8000 && p && p.ok === false && p.status === 'error' && (p.errors[0] || '').length > 0,
      `took ${took}ms, status ${p && p.status}`);

    // Timeout clamp: config value is clamped into 3s..60s; garbage -> default;
    // a per-provider override gets the SAME clamp.
    const lo = new SubsTracker(baseCfg({ fetchTimeoutMs: 1 }));
    const hi = new SubsTracker(baseCfg({ fetchTimeoutMs: 999999 }));
    const bad = new SubsTracker(baseCfg({ fetchTimeoutMs: 'x' }));
    const ov = new SubsTracker(baseCfg());
    check('subs: fetchTimeoutMs clamps to 3s..60s, garbage -> 20s default',
      lo._timeoutFor(null) === 3000 && hi._timeoutFor(null) === 60000 && bad._timeoutFor(null) === 20000 &&
      ov._timeoutFor({ timeoutMs: 80 }) === 3000 && ov._timeoutFor({ timeoutMs: 9999999 }) === 60000);

    // Last-good retention: a good cycle followed by a failing cycle keeps the
    // windows on the board, marked stale, instead of wiping the provider.
    const goodBody = { plan_type: 'pro', rate_limit: { primary_window: { used_percent: 20, reset_at: 1900000000 } } };
    let fail = false;
    const flaky = async (u, o) => {
      if (fail) throw new Error('boom');
      return { ok: true, json: async () => goodBody };
    };
    const f = new SubsTracker(baseCfg(), flaky);
    f.cfg.providers = [chatgptProvider({ timeoutMs: 500 })];
    const good = await f.rescan();
    const goodP = good.providers['chatgpt:0'];
    fail = true;
    const after = await f.rescan();
    const afterP = after.providers['chatgpt:0'];
    check('subs: failed cycle keeps last good windows marked stale',
      goodP.ok === true && goodP.windows.length === 1 &&
      afterP.ok === false && afterP.windows.length === 1 &&
      afterP.windows[0].percent === 20 &&
      afterP.status === 'stale');

    // A DISABLED provider must not keep stale windows on the board: switching
    // an entry off shows its disabled state instead of frozen numbers.
    fail = false;
    const g = new SubsTracker(baseCfg(), flaky);
    g.cfg.providers = [chatgptProvider({ timeoutMs: 500 })];
    await g.rescan();
    g.cfg.providers = [chatgptProvider({ enabled: false })];
    const dis = await g.rescan();
    const disP = dis.providers['chatgpt:0'];
    check('subs: disabled provider shows disabled state, no stale windows',
      disP.ok === false && disP.windows.length === 0 &&
      disP.status === 'disabled');

    // A disabled tracker never fetches at all (master off = zero requests).
    let called = 0;
    const counting = async () => { called++; throw new Error('should not be called'); };
    const off = new SubsTracker({ ...DEFAULTS, subs: { ...baseCfg().subs, enabled: false } }, counting);
    await off.rescan();
    check('subs: master off -> no fetches, snapshot disabled', called === 0 && off.snapshot().enabled === false);
  })().catch((e) => check('subs: block', false, e.message))
    .finally(asyncFinished);
}

// --- 11. dashboard section toggles -------------------------------------------
{
  const { DEFAULTS } = require('../src/config');
  const d = DEFAULTS.tokens.dashboard;
  check('dash toggles: all six sections default visible',
    d && d.stats === true && d.heatmap === true && d.dayDetail === true &&
    d.apps === true && d.models === true && d.plans === true, JSON.stringify(d));
  // Config round trip: a user file turning one toggle off must keep the rest
  // on (deepMerge over DEFAULTS), and subs width/height survive the merge.
  fs.mkdirSync(path.join(FAKE_HOME, '.wizbar'), { recursive: true });
  fs.writeFileSync(path.join(FAKE_HOME, '.wizbar', 'config.json'), JSON.stringify({
    tokens: { dashboard: { heatmap: false } },
    subs: { width: 900, height: 500 }
  }));
  pendingAsync++;
  (() => {
    try {
      const { ConfigManager } = require('../src/config');
      const mgr = new ConfigManager();
      const merged = mgr.load();
      if (mgr._watcher) { try { mgr._watcher.close(); } catch (_) {} }
      check('dash toggles: user override merges, unset sections stay visible',
        merged.tokens.dashboard.heatmap === false && merged.tokens.dashboard.stats === true &&
        merged.tokens.dashboard.models === true);
      check('subs: board size overrides merge with defaults',
        merged.subs.width === 900 && merged.subs.height === 500 && merged.subs.fetchTimeoutMs === 20000);
    } catch (e) {
      check('dash toggles: config round trip', false, e.message);
    }
    asyncFinished();
  })();
}

// --- 12. subscription board renderer: emitted ids match the lookups ----------
// Runs renderer/subs.js against a minimal DOM stub whose getElementById is an
// EXACT id map (like the real DOM): an emitter/lookup mismatch (the CSS.escape
// bug class) resolves to null and the population assertions fail.
{
  const vm = require('vm');
  const byId = {};
  const lookedUp = [];
  function mkEl() {
    const kids = {};
    const el = {
      textContent: '', className: '', style: {}, dataset: {},
      classList: { toggle() {}, add() {}, remove() {} },
      querySelector(sel) { return kids[sel] || (kids[sel] = mkEl()); },
      querySelectorAll() { return []; },
      addEventListener() {}
    };
    // innerHTML assignment materializes elements with ids, like a real DOM:
    // this is what makes the exact-id lookup assertions meaningful.
    let html = '';
    Object.defineProperty(el, 'innerHTML', {
      get: () => html,
      set(v) {
        html = v;
        for (const m of String(v).matchAll(/id="([^"]+)"/g)) {
          if (!byId[m[1]]) byId[m[1]] = mkEl();
        }
      }
    });
    return el;
  }
  for (const id of ['btn-close', 'btn-refresh', 'panels', 'notes', 'scan-info']) byId[id] = mkEl();
  global.document = {
    getElementById(id) { lookedUp.push(id); return byId[id] || null; },
    addEventListener() {}
  };
  const realSetInterval = global.setInterval;
  global.setInterval = () => 0; // renderer's refresh timer must not hold the process
  global.window = { __hmRamp: ['#1', '#2', '#3', '#4', '#5'], wizbar: {
    close() {}, rescanSubs: () => Promise.resolve(null),
    onTheme() {}, onSubs() {},
    getTheme: () => Promise.resolve(null), getSubs: () => Promise.resolve(null)
  } };
  pendingAsync++;
  (() => {
    try {
      let api = null;
      const src = fs.readFileSync(path.join(__dirname, '..', 'renderer', 'subs.js'), 'utf8')
        + '\n;__api = { panelHtml, renderProvider, render, setState: (s) => { state = s; } };';
      global.__api = null;
      vm.runInThisContext(src, { filename: 'renderer/subs.js' });
      api = global.__api;
      const provider = {
        ok: true, label: 'Test', plan: 'pro', status: 'ok', errors: [], notes: [],
        fetchedAt: Date.now(),
        windows: [{ key: 'primary', label: '5h', percent: 20, remainingPercent: 80,
          used: 100, total: 500, remaining: 400, resetAt: Date.now() + 3600000 }]
      };
      // panelHtml emits the panel markup; renderProvider must find it by the
      // SAME id (provider ids contain a colon).
      const html = api.panelHtml('chatgpt:0', provider, 0);
      const emitted = [...html.matchAll(/id="([^"]+)"/g)].map((m) => m[1])
        .filter((id) => id.startsWith('panel-'));
      global.state = { enabled: true, providers: { 'chatgpt:0': provider }, lastScan: new Date().toISOString() };
      api.setState(global.state);
      api.render();
      const panelEl = byId[emitted[0]]; // exact id map, like the real DOM
      check('subs board: panelHtml emits one panel per provider id',
        emitted.length === 1 && emitted[0] === 'panel-chatgpt:0', JSON.stringify(emitted));
      check('subs board: renderProvider populates the EMITTED panel (exact id lookup)',
        !!panelEl && panelEl.querySelector('[data-plan]').textContent === 'pro' &&
        panelEl.querySelector('[data-pill]').textContent === 'ok' &&
        panelEl.querySelector('[data-pies]').innerHTML.includes('<svg'),
        `looked up: ${JSON.stringify(lookedUp)}`);
      // A status with no pill wording hides the capsule instead of an empty blob.
      provider.status = 'unknown';
      api.render();
      check('subs board: unlabeled status renders an empty pill',
        byId[emitted[0]].querySelector('[data-pill]').textContent === '');
    } catch (e) {
      check('subs board: renderer block', false, e.message);
    } finally {
      global.setInterval = realSetInterval;
      delete global.__api;
      delete global.state;
      asyncFinished();
    }
  })();
}

function done() {
  if (failures) { console.error(`\n${failures} FAILURE(S)`); process.exit(1); }
  console.log('\nAll portable regression checks passed.');
  process.exit(0);
}