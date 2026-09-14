'use strict';
// Smoke test for the dock/topmost fix primitives: drive a throwaway
// BrowserWindow through native.setToolWindow / setTopmost and assert the
// WS_EX_TOOLWINDOW / WS_EX_TOPMOST ex-style bits actually land.
//
// Run: electron scripts/toolwindow_smoke.js
const { app, BrowserWindow } = require('electron');
const native = require('../src/native');
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

app.whenReady().then(() => {
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

  console.log(failed ? 'FAILED: ' + failed + ' check(s)' : 'ALL CHECKS PASSED');
  app.exit(failed ? 1 : 0);
});
