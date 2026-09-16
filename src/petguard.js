'use strict';
// Guardian for the desktop pet: it must never end up behind another
// window. Two distinct ways it gets buried, and why a topmost-bit check alone
// misses the second one:
//
//   1. It loses its topmost band - something calls SetWindowPos on it without
//      the topmost flag (fullscreen apps, some launchers, its own init), and
//      WS_EX_TOPMOST reads back clear. Nothing restores it, so the occlusion
//      sticks until the user clicks the pet.
//   2. It KEEPS the bit and is still not in front. Among topmost windows the
//      one raised last wins, so a window that raises itself afterwards sits over
//      the pet while isTopmost() keeps reporting true. Clicking the pet
//      "levitates" it - that raise happens inside the topmost band, with no bit
//      ever changing.
//
// Both are answered the same way: SetWindowPos(HWND_TOPMOST), which puts it at
// the top of the topmost band. The reference window is deliberately the
// FOREGROUND window only - the shell's own windows (Shell_TrayWnd and its
// helpers) are topmost too, and re-raising above everything in the band would
// have it fighting the taskbar for no reason. The pet already sits below the
// taskbar in practice and WindowFromPoint confirms clicks in their overlap
// still reach the taskbar, so leaving that ordering alone is safe.
const native = require('./native');

const NOTHING = { hwnd: 0, action: null, reason: null };

// Re-assert if needed. `refHwnd` defaults to whatever is in the foreground now.
function ensureTopmost(pid, refHwnd) {
  let snap;
  try {
    const ref = refHwnd != null ? refHwnd : native.getForegroundWindow();
    snap = native.petGuardSnapshot(pid, ref);
  } catch (_) {
    return NOTHING;
  }
  if (!snap || !snap.hwnd) return NOTHING;
  if (!snap.topmost) {
    native.setTopmost(snap.hwnd, true);
    return { hwnd: snap.hwnd, action: 'raised', reason: 'topmost bit was lost' };
  }
  if (snap.belowRef) {
    native.setTopmost(snap.hwnd, true);
    return { hwnd: snap.hwnd, action: 'raised', reason: 'below the foreground window' };
  }
  return { hwnd: snap.hwnd, action: null, reason: null };
}

module.exports = { ensureTopmost };
