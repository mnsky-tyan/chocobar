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
    DEFAULTS.modules.remielle.enabled === false && DEFAULTS.modules.remielle.exePath === '' &&
    !('agents' in DEFAULTS.modules));
  const blob = JSON.stringify(DEFAULTS);
  check('config: no personal identifiers in defaults',
    !/tyanw|tyan|mnsky|firstmate|remielle-win|Little-Remielle/i.test(blob));
}

// --- 5. tokens aggregate empty state ------------------------------------------
{
  const { TokenTracker } = require('../src/tokens');
  const { DEFAULTS } = require('../src/config');
  const t = new TokenTracker({ ...DEFAULTS, tokens: DEFAULTS.tokens });
  const agg = t.aggregate();
  check('tokens: empty state is zeros + sourcesEnabled 0',
    agg.recordCount === 0 && agg.today.total === 0 && agg.sourcesEnabled === 0);
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
      done();
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

function done() {
  if (failures) { console.error(`\n${failures} FAILURE(S)`); process.exit(1); }
  console.log('\nAll portable regression checks passed.');
  process.exit(0);
}