'use strict';
// Win32 interop via koffi: window tracking helpers, battery, master volume.
// NOTE: koffi crashes if you call other native functions from inside an EnumWindows
// callback, so enumeration is two-pass: collect hwnds in the callback, classify after.
const koffi = require('koffi');

const user32 = koffi.load('user32.dll');
const dwmapi = koffi.load('dwmapi.dll');
const kernel32 = koffi.load('kernel32.dll');
const ole32 = koffi.load('ole32.dll');

const RECT = koffi.struct('RECT', { left: 'long', top: 'long', right: 'long', bottom: 'long' });

const EnumCb = koffi.proto('int __stdcall EnumCb(uintptr_t hwnd, void *lparam)');
const EnumWindows = user32.func('int __stdcall EnumWindows(EnumCb *cb, void *lParam)');
const GetWindowThreadProcessId = user32.func('uintptr_t __stdcall GetWindowThreadProcessId(uintptr_t hwnd, _Out_ uint32_t *pid)');
const GetClassNameW = user32.func('int __stdcall GetClassNameW(uintptr_t hwnd, _Out_ uint16_t *lpClassName, int nMaxCount)');
const GetWindowRect = user32.func('int __stdcall GetWindowRect(uintptr_t hwnd, _Out_ RECT *lpRect)');
const GetClientRect = user32.func('int __stdcall GetClientRect(uintptr_t hwnd, _Out_ RECT *lpRect)');
const IsWindowVisible = user32.func('int __stdcall IsWindowVisible(uintptr_t hwnd)');
const IsIconic = user32.func('int __stdcall IsIconic(uintptr_t hwnd)');
const GetWindowTextLengthW = user32.func('int __stdcall GetWindowTextLengthW(uintptr_t hwnd)');
const IsWindow = user32.func('int __stdcall IsWindow(uintptr_t hwnd)');
const SetWindowPos = user32.func('int __stdcall SetWindowPos(uintptr_t hwnd, uintptr_t after, int x, int y, int cx, int cy, int flags)');
const DwmGetWindowAttribute = dwmapi.func('long __stdcall DwmGetWindowAttribute(uintptr_t hwnd, int attr, _Out_ int *pvAttr, int cbAttr)');
const DwmGetWindowAttributeRect = dwmapi.func('long __stdcall DwmGetWindowAttribute(uintptr_t hwnd, int attr, _Out_ RECT *pvAttr, int cbAttr)');
const DwmSetWindowAttribute = dwmapi.func('long __stdcall DwmSetWindowAttribute(uintptr_t hwnd, int attr, int *pvAttr, int cbAttr)');
const DWMWA_CLOAKED = 14;
const DWMWA_EXTENDED_FRAME_BOUNDS = 9;   // visible frame (excludes invisible resize borders)
const DWMWA_WINDOW_CORNER_PREFERENCE = 33;
const DWMWCP_ROUND = 2;

const SYSTEM_POWER_STATUS = koffi.struct('SYSTEM_POWER_STATUS', {
  ACLineStatus: 'uint8', BatteryFlag: 'uint8', BatteryLifePercent: 'uint8',
  Reserved1: 'uint8', BatteryLifeTime: 'uint32', BatteryFullLifeTime: 'uint32'
});
const GetSystemPowerStatus = kernel32.func('int __stdcall GetSystemPowerStatus(_Out_ SYSTEM_POWER_STATUS *sps)');
const CoInitializeEx = ole32.func('long __stdcall CoInitializeEx(void *pvReserved, int dwCoInit)');
const GUID = koffi.struct('GUID', { Data1: 'uint32', Data2: 'uint16', Data3: 'uint16', Data4: 'uint8[8]' });
const CoCreateInstance = ole32.func('long __stdcall CoCreateInstance(const GUID *rclsid, void *pUnkOuter, int dwClsContext, const GUID *riid, _Out_ void **ppv)');

// --- window helpers -----------------------------------------------------------

const _clsBuf = Buffer.alloc(1024);

function getClassName(hwnd) {
  const n = GetClassNameW(hwnd, _clsBuf, 512);
  if (n <= 0) return '';
  return _clsBuf.toString('utf16le', 0, n * 2);
}

function getWindowRect(hwnd) {
  const rc = {};
  if (!GetWindowRect(hwnd, rc)) return null;
  return rc;
}

// Visible frame bounds (what the user perceives as the window edge).
function getFrameBounds(hwnd) {
  const rc = {};
  if (DwmGetWindowAttributeRect(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, rc, koffi.sizeof(RECT)) === 0) {
    return rc;
  }
  return getWindowRect(hwnd);
}

// Place `hwnd` immediately above `afterHwnd` in z-order (same band, not topmost).
const SWP_NOMOVE = 0x2, SWP_NOSIZE = 0x1, SWP_NOACTIVATE = 0x10;
function setWindowPosAfter(hwnd, afterHwnd) {
  return !!SetWindowPos(hwnd, afterHwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Windows Terminal keeps an invisible DRAG_BAR_WINDOW_CLASS overlay along its
// top edge. It sits above the terminal in z-order and eats every mouse click
// aimed at the bar's strip. Inserting the bar above the terminal alone is not
// enough — the bar must go above the drag bar too, or chip clicks land on WT.
function raiseAboveTerminalChrome(barHwnd, terminalHwnd) {
  const pidBuf = [0];
  GetWindowThreadProcessId(terminalHwnd, pidBuf);
  const wtPid = pidBuf[0];
  const fb = getFrameBounds(terminalHwnd);
  if (!fb) return false;

  const hwnds = [];
  EnumWindows((h) => { hwnds.push(Number(h)); return 1; }, null);
  let target = terminalHwnd;
  for (const h of hwnds) {
    try {
      if (!IsWindowVisible(h)) continue;
      if (getClassName(h) !== 'DRAG_BAR_WINDOW_CLASS') continue;
      const p = [0];
      GetWindowThreadProcessId(h, p);
      if (p[0] !== wtPid) continue;
      const r = getWindowRect(h);
      // must be the drag bar lying on THIS terminal's top edge
      if (r.left < fb.right && r.right > fb.left && r.top < fb.top && r.bottom > fb.top - 80) {
        target = h;
        break;
      }
    } catch (_) {}
  }
  return !!SetWindowPos(barHwnd, target, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Bring a window to the very front AND give it focus, from a background
// process. Windows denies SetForegroundWindow to processes that didn't receive
// the last input — the classic workaround is a synthetic ALT tap, which grants
// the calling process foreground rights for the next call.
const keybd_event = user32.func('void __stdcall keybd_event(uint8_t key, uint8_t scan, uint32_t flags, uintptr_t extra)');
const SetForegroundWindow = user32.func('int __stdcall SetForegroundWindow(uintptr_t hwnd)');
const GetForegroundWindow = user32.func('uintptr_t __stdcall GetForegroundWindow()');
const HWND_TOP = 0;
function bringToFront(hwnd) {
  try {
    keybd_event(0x12, 0, 0, 0);   // ALT down
    keybd_event(0x12, 0, 2, 0);   // ALT up (KEYEVENTF_KEYUP)
    SetForegroundWindow(hwnd);
    // raise above all normal windows regardless of whether focus stuck
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  } catch (_) {}
  return true;
}

// OS-rounded corners (Win11). Returns false on systems without the attribute.
const DWMWCP_DONOTROUND = 1;
function setCornerPreference(hwnd, round) {
  const pref = [round ? DWMWCP_ROUND : DWMWCP_DONOTROUND];
  return DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, pref, 4) === 0;
}
function roundCorners(hwnd) {
  return setCornerPreference(hwnd, true);
}

// Force the window's DWM backdrop (backgroundMaterial) out of dark mode, otherwise
// the system acrylic smokes dark on machines using the dark theme.
const DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
function setImmersiveDarkMode(hwnd, isDark) {
  const val = [isDark ? 1 : 0];
  return DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, val, 4) === 0;
}

// Win11 draws a 1px grey border stroke around rounded windows — kill it.
const DWMWA_BORDER_COLOR = 34;
const DWMWA_COLOR_NONE = 0xFFFFFFFE;
function removeBorderColor(hwnd) {
  const val = [DWMWA_COLOR_NONE >>> 0];
  try {
    return DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, val, 4) === 0;
  } catch (_) {
    return false; // attr needs a real color value on older builds
  }
}

// --- Windows-Terminal-style acrylic (SetWindowCompositionAttribute) -------------
// NOTE: tried for the bar — renders MOSAIC/blocky on Win11. The bar now uses
// backgroundMaterial + setImmersiveDarkMode(false) instead. Kept here only as
// reference for future experiments.
const ACCENT_POLICY = koffi.struct('ACCENT_POLICY', {
  AccentState: 'int', AccentFlags: 'int', GradientColor: 'uint32', AnimationId: 'int'
});
const WINCOMPATTRDATA = koffi.struct('WINCOMPATTRDATA', {
  Attribute: 'int', Data: koffi.pointer(ACCENT_POLICY), SizeOfData: 'int'
});
const SetWindowCompositionAttribute = user32.func('int __stdcall SetWindowCompositionAttribute(uintptr_t hwnd, WINCOMPATTRDATA *data)');

function hwndNumberFromBuffer(buf) {
  if (!buf || buf.length < 4) return null;
  try { return Number(buf.readBigUInt64LE(0)); } catch { return null; }
}

function isCloaked(hwnd) {
  const out = [0];
  const hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, out, 4);
  return hr === 0 && out[0] !== 0;
}

function listWindowsByClass(className) {
  const hwnds = [];
  EnumWindows((hwnd) => { hwnds.push(Number(hwnd)); return 1; }, null);
  const found = [];
  for (const hwnd of hwnds) {
    try {
      if (!IsWindowVisible(hwnd)) continue;
      if (getClassName(hwnd) !== className || isCloaked(hwnd)) continue;
      // Windows Terminal keeps ghost clones (no title, stale on-screen rect,
      // invisible yet "visible" to every DWM check) that hijack the tracker —
      // following one hides the bar forever. A real terminal always has a
      // title (e.g. "MNSKY_LAPTOP: harness") and is never iconic while shown.
      if (!GetWindowTextLengthW(hwnd)) continue;
      if (IsIconic(hwnd)) continue;
      // Minimized windows park at physical (-32000,-32000); nothing real is
      // ever fully offscreen, so require a sane rect as well.
      const rc = getWindowRect(hwnd);
      if (!rc) continue;
      if (rc.left <= -16000 || rc.top <= -16000) continue;
      if (rc.right - rc.left < 100 || rc.bottom - rc.top < 100) continue;
      found.push(hwnd);
    } catch (_) {}
  }
  return found;
}

// --- battery -------------------------------------------------------------------

function getBattery() {
  const sps = {};
  try {
    if (!GetSystemPowerStatus(sps)) return null;
    return {
      percent: sps.BatteryLifePercent <= 100 ? sps.BatteryLifePercent : null,
      ac: sps.ACLineStatus === 1,
      // BatteryFlag is unreliable (Windows keeps 0x8 set after unplug and
      // drops it at 100% on AC). Gate on ACLineStatus: charging = on AC and
      // not yet full.
      charging: sps.ACLineStatus === 1 && (sps.BatteryFlag & 0x8) !== 0 &&
        sps.BatteryLifePercent < 100,
      noBattery: (sps.BatteryFlag & 0x80) !== 0
    };
  } catch {
    return null;
  }
}

// --- master volume via Core Audio (IAudioEndpointVolume) ------------------------

const CLSID_MMDeviceEnumerator = { Data1: 0xBCDE0395, Data2: 0xE52F, Data3: 0x467C, Data4: [0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E] };
const IID_IMMDeviceEnumerator = { Data1: 0xA95664D2, Data2: 0x9614, Data3: 0x4F35, Data4: [0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6] };
const IID_IAudioEndpointVolume = { Data1: 0x5CDF2C82, Data2: 0x841E, Data3: 0x4546, Data4: [0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A] };

const volumeState = { ok: false, endpointVolume: null, error: null };

const protoGetDefaultEP = koffi.proto('long __stdcall GetDefaultAudioEndpoint(void *self, int flow, int role, _Out_ void **ppDevice)');
const protoActivate = koffi.proto('long __stdcall Activate(void *self, const GUID *iid, int clsCtx, void *activationParams, _Out_ void **ppInterface)');
const protoGetVolume = koffi.proto('long __stdcall GetMasterVolumeLevelScalar(void *self, _Out_ float *pfLevel)');
const protoGetMute = koffi.proto('long __stdcall GetMute(void *self, _Out_ int *pbMute)');

function initVolume(role) {
  try { CoInitializeEx(null, 0x2 /* APARTMENTTHREADED */); } catch (_) { /* already initialized - fine */ }

  const pEnum = [null];
  const hr = CoCreateInstance(CLSID_MMDeviceEnumerator, null, 0x17 /* CLSCTX_ALL */, IID_IMMDeviceEnumerator, pEnum);
  if (hr !== 0 || !pEnum[0]) { volumeState.error = 'CoCreateInstance hr=' + hr; return false; }

  // IMMDeviceEnumerator::GetDefaultAudioEndpoint = vtable slot 4; eRender=0
  const vtEnum = koffi.decode(pEnum[0], 0, 'void *');
  const pGetDefault = koffi.decode(vtEnum, 4 * 8, 'void *');
  const device = [null];
  const roleIdx = role === 'console' ? 0 : role === 'communications' ? 2 : 1; // eMultimedia
  const hr2 = koffi.call(pGetDefault, protoGetDefaultEP, pEnum[0], 0, roleIdx, device);
  if (hr2 !== 0 || !device[0]) { volumeState.error = 'GetDefaultAudioEndpoint hr=' + hr2; return false; }

  // IMMDevice::Activate = vtable slot 3
  const vtDev = koffi.decode(device[0], 0, 'void *');
  const pActivate = koffi.decode(vtDev, 3 * 8, 'void *');
  const epv = [null];
  const hr3 = koffi.call(pActivate, protoActivate, device[0], IID_IAudioEndpointVolume, 1 /* CLSCTX_INPROC_SERVER */, null, epv);
  if (hr3 !== 0 || !epv[0]) { volumeState.error = 'Activate hr=' + hr3; return false; }
  volumeState.endpointVolume = epv[0];
  volumeState.ok = true;
  return true;
}

function getVolume() {
  if (!volumeState.ok) return null;
  try {
    const vt = koffi.decode(volumeState.endpointVolume, 0, 'void *');
    const pGetVol = koffi.decode(vt, 9 * 8, 'void *');
    const pGetMute = koffi.decode(vt, 15 * 8, 'void *');
    const lvl = [0];
    const mute = [0];
    if (koffi.call(pGetVol, protoGetVolume, volumeState.endpointVolume, lvl) !== 0) return null;
    koffi.call(pGetMute, protoGetMute, volumeState.endpointVolume, mute);
    return { level: Math.round(lvl[0] * 100), muted: mute[0] !== 0 };
  } catch (e) {
    volumeState.ok = false;
    volumeState.error = e.message;
    return null;
  }
}

function getClientRect(hwnd) {
  const rc = {};
  if (!GetClientRect(hwnd, rc)) return null;
  return rc; // client-area size, origin always 0,0
}

function forceSize(hwnd, wPhys, hPhys) {
  const SWP_NOMOVE = 0x2, SWP_NOZORDER = 0x4, SWP_NOACTIVATE = 0x10;
  return !!SetWindowPos(hwnd, 0, 0, 0, wPhys, hPhys, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

module.exports = {
  getClassName, getWindowRect, getFrameBounds, getClientRect, forceSize, isCloaked, listWindowsByClass,
  setWindowPosAfter, raiseAboveTerminalChrome, roundCorners, setCornerPreference, setImmersiveDarkMode, removeBorderColor, hwndNumberFromBuffer, bringToFront,
  isIconic: (h) => !!IsIconic(h), isWindow: (h) => !!IsWindow(h), isVisible: (h) => !!IsWindowVisible(h),
  getBattery, initVolume, getVolume, volumeState,
  getForegroundWindow: () => GetForegroundWindow()
};
