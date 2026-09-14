'use strict';
// Smoke test for the dock/topmost fix primitives and the pet guardian: drive
// throwaway BrowserWindows through native.setToolWindow / setTopmost and assert
// the WS_EX_TOOLWINDOW / WS_EX_TOPMOST ex-style bits actually land, then replay
// the two ways 小雷米 ends up behind another window. The guardian section shows
// two small windows for about a second.
//
// Run: electron scripts/toolwindow_smoke.js
const { app, BrowserWindow } = require('electron');
const native = require('../src/native');
const petguard = require('../src/petguard');
const koffi = require('koffi');

const user32 = koffi.load('user32.dll');
const GetWindowLongW = user32.func('long __stdcall GetWindowLongW(uintptr_t hwnd, int nIndex)');
const GWL_EXSTYLE = -20;
const WS_EX_TOOLWINDOW = 0x80;
const WS_EX_TOPMOST = 0x8;

let failed = 0;
function check(name, cond, detail) {
  console.log((cond ? 'PASS' : 'FAIL') + ': ' + name + (detail ? '  [' + detail + ']' : ''));
  if (!cond) failed++;
}

app.whenReady().then(async () => {
  const win = new BrowserWindow({ width: 300, height: 100, show: false, frame: false, skipTaskbar: true });
  const hwnd = native.hwndNumberFromBuffer(win.getNativeWindowHandle());
  check('hwnd resolved', !!hwnd, String(hwnd));

  const ex0 = GetWindowLongW(hwnd, GWL_EXSTYLE);
  check('toolwindow absent before assert', (ex0 & WS_EX_TOOLWINDOW) === 0, 'ex=0x' + ex0.toString(16));

  check('setToolWindow(true) returns true', native.setToolWindow(hwnd, true) === true);
  const ex1 = GetWindowLongW(hwnd, GWL_EXSTYLE);
  check('WS_EX_TOOLWINDOW set', (ex1 & WS_EX_TOOLWINDOW) !== 0, 'ex=0x' + ex1.toString(16));

  check('setToolWindow(true) idempotent', native.setToolWindow(hwnd, true) === true);
  const ex2 = GetWindowLongW(hwnd, GWL_EXSTYLE);
  check('style unchanged by idempotent call', ex2 === ex1);

  // the bar's assertNoTaskbar path: Electron's DeleteTab + our toolwindow together
  win.setSkipTaskbar(true);
  check('skipTaskbar coexists with toolwindow', (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) !== 0);

  check('isTopmost false initially', native.isTopmost(hwnd) === false);
  check('setTopmost(true) returns true', native.setTopmost(hwnd, true) === true);
  check('isTopmost true after setTopmost', native.isTopmost(hwnd) === true);

  check('setToolWindow(false) clears the bit', native.setToolWindow(hwnd, false) === true);
  const ex3 = GetWindowLongW(hwnd, GWL_EXSTYLE);
  check('toolwindow bit cleared', (ex3 & WS_EX_TOOLWINDOW) === 0, 'ex=0x' + ex3.toString(16));
  check('topmost survived unrelated style write', native.isTopmost(hwnd) === true);

  // --- pet guardian: the case a topmost-bit check alone misses ---------------
  // Two topmost windows z-fight and the one raised last is in front. The pet
  // keeps WS_EX_TOPMOST the whole time, so isTopmost() reports "fine" while she
  // sits behind the other window - the captain's "she hides until I click her".
  const petWin = new BrowserWindow({ width: 420, height: 300, x: 60, y: 60, frame: false, skipTaskbar: true, show: false });
  const otherWin = new BrowserWindow({ width: 260, height: 180, x: 560, y: 60, frame: false, skipTaskbar: true, show: false });
  petWin.showInactive();
  otherWin.showInactive();
  await new Promise((r) => setTimeout(r, 500));
  const petHwnd = native.hwndNumberFromBuffer(petWin.getNativeWindowHandle());
  const otherHwnd = native.hwndNumberFromBuffer(otherWin.getNativeWindowHandle());

  check('guard resolves the pet window by pid',
    native.petGuardSnapshot(process.pid, otherHwnd).hwnd === petHwnd, 'want=' + petHwnd);

  check('setTopmost(pet, true)', native.setTopmost(petHwnd, true) === true);
  check('setTopmost(other, true) - the other window raised last', native.setTopmost(otherHwnd, true) === true);
  check('pet kept its topmost bit', native.isTopmost(petHwnd) === true);
  check('pet is behind the other window in z-order', native.isBelowInZOrder(petHwnd, otherHwnd) === true);

  const r1 = petguard.ensureTopmost(process.pid, otherHwnd);
  check('guard reports the pet below the foreground window',
    r1.reason === 'below the foreground window', JSON.stringify(r1));
  check('pet is in front again', native.isBelowInZOrder(petHwnd, otherHwnd) === false);

  const r2 = petguard.ensureTopmost(process.pid, otherHwnd);
  check('guard is a no-op once she is in front', r2.action === null, JSON.stringify(r2));

  const r3 = petguard.ensureTopmost(process.pid, petHwnd);
  check('pet as its own reference is not a violation', r3.action === null, JSON.stringify(r3));

  check('setTopmost(pet, false) drops her out of the topmost band', native.setTopmost(petHwnd, false) === true);
  check('topmost bit is really gone', native.isTopmost(petHwnd) === false);
  const r4 = petguard.ensureTopmost(process.pid, otherHwnd);
  check('guard restores a lost topmost bit', r4.reason === 'topmost bit was lost', JSON.stringify(r4));
  check('topmost bit is back', native.isTopmost(petHwnd) === true);
  check('pet is above the other topmost window', native.isBelowInZOrder(petHwnd, otherHwnd) === false);

  check('foreground window resolves to a handle', Number(native.getForegroundWindow()) > 0);

  // The guard re-resolves the window every tick (no cached hwnd), so a pet whose
  // window is re-created is picked up again instead of being guarded forever.
  petWin.destroy();
  const reborn = new BrowserWindow({ width: 420, height: 300, x: 60, y: 60, frame: false, skipTaskbar: true, show: false });
  reborn.showInactive();
  await new Promise((r) => setTimeout(r, 500));
  const rebornHwnd = native.hwndNumberFromBuffer(reborn.getNativeWindowHandle());
  const snap2 = native.petGuardSnapshot(process.pid, otherHwnd);
  check('guard picks up a re-created pet window', snap2.hwnd === rebornHwnd, 'want=' + rebornHwnd);
  check('the re-created window is not topmost yet', snap2.topmost === false);
  const r5 = petguard.ensureTopmost(process.pid, otherHwnd);
  check('guard asserts topmost on the re-created window',
    r5.reason === 'topmost bit was lost', JSON.stringify(r5));
  reborn.destroy();
  otherWin.destroy();

  console.log(failed ? 'FAILED: ' + failed + ' check(s)' : 'ALL CHECKS PASSED');
  app.exit(failed ? 1 : 0);
});
