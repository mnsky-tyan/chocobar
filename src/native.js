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
const GetWindowLongW = user32.func('long __stdcall GetWindowLongW(uintptr_t hwnd, int nIndex)');
const SetWindowLongW = user32.func('long __stdcall SetWindowLongW(uintptr_t hwnd, int nIndex, long dwNewLong)');
const GWL_EXSTYLE = -20;
const WS_EX_TOPMOST = 0x8;
const WS_EX_TOOLWINDOW = 0x80;
function isTopmost(hwnd) {
  try { return (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) !== 0; } catch (_) { return false; }
}
// WS_EX_TOOLWINDOW: the STRUCTURAL "never in the taskbar" bit. Electron's
// setSkipTaskbar is a stateless ITaskbarList::DeleteTab - the moment the shell
// re-enumerates windows (explorer restart, taskbar rebuild, sleep/wake, display
// topology change) it re-adds a button for every eligible window and the
// deletion is forgotten. The ex-style is re-read on every enumeration, so it
// survives all of those. Cheap enough to re-assert on a timer; no-op write
// when the bit is already correct.
function setToolWindow(hwnd, enable) {
  try {
    const ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    const next = enable ? (ex | WS_EX_TOOLWINDOW) : (ex & ~WS_EX_TOOLWINDOW);
    if (next === ex) return true;
    SetWindowLongW(hwnd, GWL_EXSTYLE, next);
    return (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) === (enable ? WS_EX_TOOLWINDOW : 0);
  } catch (_) { return false; }
}
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

// True when `hwndA` currently sits BELOW `hwndB` in the z-order (EnumWindows
// walks top→bottom). False when either window is absent from the enumeration.
function isBelowInZOrder(hwndA, hwndB) {
  const hwnds = [];
  EnumWindows((h) => { hwnds.push(Number(h)); return 1; }, null);
  let ia = -1, ib = -1;
  for (let i = 0; i < hwnds.length && (ia < 0 || ib < 0); i++) {
    if (hwnds[i] === hwndA && ia < 0) ia = i;
    if (hwnds[i] === hwndB && ib < 0) ib = i;
  }
  return ia >= 0 && ib >= 0 && ia > ib;
}

// Band movers. HWND_TOPMOST(-1) lifts the window into the topmost band (above
// every normal window); HWND_NOTOPMOST(-2) drops it back to the normal band.
// 64-bit handles are sign-extended, so the constants must go in as full-width
// BigInts (0xFFFF...FF) — 0xFFFFFFFF truncates to an invalid handle value and
// SetWindowPos fails with ERROR_INVALID_HANDLE.
// SetWindowLongW(GWL_EXSTYLE) is not an alternative: Windows silently ignores
// WS_EX_TOPMOST in that write (measured - the read-back bit does not change),
// so only SetWindowPos moves a window between the two bands.
const HWND_TOPMOST_B = (1n << 64n) - 1n;   // -1
const HWND_NOTOPMOST_B = (1n << 64n) - 2n; // -2
function setTopmost(hwnd, topmost) {
  const insert = topmost ? HWND_TOPMOST_B : HWND_NOTOPMOST_B;
  return !!SetWindowPos(hwnd, insert, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Diagnostics: the raw positions of two hwnds in OUR EnumWindows walk plus the
// total window count, so a disagreeing external observer can be debugged.
function debugZOrder(hwndA, hwndB) {
  const hwnds = [];
  EnumWindows((h) => { hwnds.push(Number(h)); return 1; }, null);
  return { n: hwnds.length, ia: hwnds.indexOf(hwndA), ib: hwnds.indexOf(hwndB) };
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

// --- CPU temperature via HWiNFO shared memory -----------------------------------
// HWiNFO publishes every sensor reading to a named section (Global\HWiNFO_SENS_SM)
// when "Shared Memory Support" is enabled in its settings. Mapping it read-only
// needs no helper and no elevation; the section dies with the HWiNFO process, so
// an absent map is the normal "not running / setting off" case and returns null.
const OpenFileMappingW = kernel32.func('uintptr_t __stdcall OpenFileMappingW(uint32_t access, int inherit, str16 name)');
const MapViewOfFile = kernel32.func('void *__stdcall MapViewOfFile(uintptr_t h, uint32_t access, uint32_t hi, uint32_t lo, size_t bytes)');
const UnmapViewOfFile = kernel32.func('int __stdcall UnmapViewOfFile(void *p)');
const CloseHandle = kernel32.func('int __stdcall CloseHandle(uintptr_t h)');
const HWI_MBI = koffi.struct('HWI_MBI', {
  BaseAddress: 'void *', AllocationBase: 'void *', AllocationProtect: 'uint32', pad1: 'uint32',
  RegionSize: 'size_t', State: 'uint32', Protect: 'uint32', Type: 'uint32', pad2: 'uint32'
});
const VirtualQuery = kernel32.func('size_t __stdcall VirtualQuery(void *addr, _Out_ HWI_MBI *mbi, size_t len)');

const HWINFO_MAP = 'Global\\HWiNFO_SENS_SM';

// Stateless by design: each poll opens, maps, parses and releases the section.
// Holding a mapping would pin a dead section object after HWiNFO restarts, and
// at a 2s cadence the three extra syscalls are free.

// Pure parser over an explicit-length byte reader so tests can drive it without a
// live section. Layout per HWiNFO's SDK: 40-byte header (signature 0x10, version,
// revision, two 8-byte timestamps, sensor and reading element sizes), then a
// NUL-terminated sensor array, then reading elements. String fields are UTF-16 or
// UTF-8 depending on revision — the element sizes tell which, and every offset
// derives from them (label field is 8x the suffix field). koffi string decodes are
// never used here: they run past mapped memory unless NUL-terminated, so all reads
// are explicit-length bytes.
function parseHwinfoCpuTemp(get, bytes) {
  try {
    if (!get || bytes < 40) return null;
    const hdr = get(0, 40);
    if (hdr.readUInt32LE(0) !== 0x10) return null;
    const sensorSize = hdr.readUInt32LE(32);
    const readingSize = hdr.readUInt32LE(36);
    // reading element = 48 bytes of scalars + label + suffix + 8 bytes of ids
    const suffixBytes = (readingSize - 56) / 9;
    if ((suffixBytes !== 16 && suffixBytes !== 32) || sensorSize !== (suffixBytes === 32 ? 512 : 256)) return null;
    const wide = suffixBytes === 32;
    const dec = (buf) => {
      const s = buf.toString(wide ? 'utf16le' : 'utf8');
      const z = s.indexOf('\0');
      return (z >= 0 ? s.slice(0, z) : s).trim();
    };

    const sensors = [];
    let off = 40, readingsAt = -1;
    for (let i = 0; i < 256; i++) {
      const name = dec(get(off, sensorSize / 2));
      if (!name) { readingsAt = off; break; }
      sensors.push(name);
      off += sensorSize;
    }
    if (readingsAt < 0) return null;

    // collect(start): walk reading elements until an empty label (fresh maps are
    // zero-filled past the live entries). The SDK leaves it ambiguous whether the
    // readings begin on the sensor terminator slot or after it, so callers try both.
    const labelBytes = suffixBytes * 8;
    const collect = (start) => {
      const temps = [];
      for (let i = 0; i < 1024; i++) {
        const ro = start + i * readingSize;
        const label = dec(get(ro + 48, labelBytes));
        if (!label) break;
        const sfx = dec(get(ro + 48 + labelBytes, suffixBytes));
        if (!sfx.includes('°')) continue;
        const v = get(ro + 16, 8).readDoubleLE(0);
        if (!(v > 0 && v < 150)) continue;
        const id = get(ro + 48 + labelBytes + suffixBytes, 4).readUInt32LE(0);
        temps.push({ c: Math.round(v * 10) / 10, label, sensor: sensors[id] || '' });
      }
      return temps;
    };
    let temps = collect(readingsAt);
    if (!temps.length) temps = collect(readingsAt + sensorSize);
    if (!temps.length) return null;
    // Prefer the CPU package temp, then a CPU-labeled reading, then the hottest core.
    const onCpu = (t) => /cpu/i.test(t.sensor);
    const pkg = temps.find((t) => onCpu(t) && /package/i.test(t.label));
    if (pkg) return pkg;
    const cpuLbl = temps.find((t) => onCpu(t) && /(^|\W)(cpu|tctl|tdie)\b/i.test(t.label) && !/distance/i.test(t.label));
    if (cpuLbl) return cpuLbl;
    const cores = temps.filter((t) => onCpu(t) && !/distance/i.test(t.label));
    if (cores.length) return cores.reduce((a, b) => (b.c > a.c ? b : a));
    return { none: true };
  } catch (_) {
    return null;
  }
}

// --- SM2 (HWiNFO 8.24+): Global\HWiNFO_SENS_SM2, signature 'HWiS' -------------
// 48-byte header: signature, version, revision, unix poll time, uptime, header
// size, sensor element size, sensor count, readings offset, reading element
// size, reading count, capacity. Sensor records carry the sensor name at +8;
// reading records (stride 460 on 8.52) carry parent sensor id at +0, label at
// +12, unit at +268, then current/min/max/avg doubles at +284..+308. Strings
// are byte-oriented with the degree sign as raw latin-1 0xB0, so decode latin1.
const HWINFO_MAP2 = 'Global\\HWiNFO_SENS_SM2';

function parseHwinfoSm2CpuTemp(get, bytes) {
  try {
    if (bytes < 48) return null;
    const hdr = get(0, 48);
    if (hdr.readUInt32LE(0) !== 0x53695748) return null; // 'HWiS'
    const sensorSize = hdr.readUInt32LE(24), sensorCount = hdr.readUInt32LE(28);
    const readingsOff = hdr.readUInt32LE(32), readingSize = hdr.readUInt32LE(36), readingCount = hdr.readUInt32LE(40);
    if (sensorSize < 136 || readingSize < 320 || !sensorCount || !readingCount) return null;
    if (readingsOff + readingCount * readingSize > bytes) return null;
    const sensors = [];
    for (let i = 0; i < sensorCount; i++) sensors.push(strLat(get(48 + i * sensorSize + 8, 120)));
    const degc = [];
    for (let j = 0; j < readingCount; j++) {
      const rec = readingsOff + j * readingSize;
      const unit = strLat(get(rec + 268, 8));
      if (unit !== '\u00B0C') continue;
      const v = get(rec + 284, 8).readDoubleLE(0);
      if (!(v > 0 && v < 150)) continue;
      degc.push({ c: Math.round(v * 10) / 10, label: strLat(get(rec + 12, 100)), sensor: sensors[get(rec, 4).readUInt32LE(0)] || '' });
    }
    if (!degc.length) return { none: true };
    const onCpu = (t) => /cpu/i.test(t.sensor);
    const pkg = degc.find((t) => onCpu(t) && /package/i.test(t.label));
    if (pkg) return pkg;
    const cpuLbl = degc.find((t) => onCpu(t) && /(^|\W)(cpu|tctl|tdie)\b/i.test(t.label) && !/distance/i.test(t.label));
    if (cpuLbl) return cpuLbl;
    const cores = degc.filter((t) => onCpu(t) && /core/i.test(t.label) && !/distance/i.test(t.label));
    if (cores.length) return cores.reduce((a, b) => (b.c > a.c ? b : a));
    return { none: true };
  } catch (_) {
    return null;
  }
}

function strLat(bytes) {
  const z = bytes.indexOf(0);
  return (z >= 0 ? bytes.toString('latin1', 0, z) : bytes.toString('latin1')).trim();
}

// Returns a STATE object, never null:
//   { state: 'ok', c, label }   live CPU temperature
//   { state: 'no-temp' }        section live but no CPU temperature reading
//   { state: 'no-section' }     HWiNFO not running, shm setting off, or HWiNFO
//                               restarting (sections die with the process)
function getHwinfoTemp(mapName) {
  const names = mapName ? [mapName] : [HWINFO_MAP2, HWINFO_MAP];
  for (const name of names) {
    let h = null, p = null;
    try {
      h = OpenFileMappingW(4 /* FILE_MAP_READ */, 0, name);
      if (!h) continue;
      p = MapViewOfFile(h, 4, 0, 0, 0);
      if (!p) continue;
      // RegionSize bounds every later read: decoding past a mapped section would crash.
      const mbi = {};
      let bytes = 0;
      if (VirtualQuery(p, mbi, koffi.sizeof(HWI_MBI)) === koffi.sizeof(HWI_MBI)) bytes = Number(mbi.RegionSize) || 0;
      if (bytes < 48) continue;
      const get = (o, n) => {
        if (o + n > bytes) throw new Error('hwinfo read past section');
        return Buffer.from(koffi.decode(p, o, 'uint8', n));
      };
      const parsed = get(0, 4).readUInt32LE(0) === 0x53695748 ? parseHwinfoSm2CpuTemp(get, bytes) : parseHwinfoCpuTemp(get, bytes);
      if (!parsed) continue; // structurally invalid: HWiNFO restarting; retry next poll
      if (parsed.none) return { state: 'no-temp' };
      return { state: 'ok', c: parsed.c, label: parsed.label };
    } catch (_) {
      continue;
    } finally {
      if (p) { try { UnmapViewOfFile(p); } catch (_) {} }
      if (h) { try { CloseHandle(h); } catch (_) {} }
    }
  }
  return { state: 'no-section' };
}

// --- process lookup by image name (Toolhelp32 snapshot) ------------------------
// pollRemielle used to spawn tasklist.exe every 3s just to learn whether the pet
// process is alive (~290ms of CPU per spawn here); an in-process snapshot costs
// ~5ms. Returns the matching pid, or null when no process carries that image name.
const TH32CS_SNAPPROCESS = 0x2;
const PROCESSENTRY32W = koffi.struct('PROCESSENTRY32W', {
  dwSize: 'uint32', cntUsage: 'uint32', th32ProcessID: 'uint32',
  th32DefaultHeapID: 'uintptr_t', th32ModuleID: 'uint32', cntThreads: 'uint32',
  th32ParentProcessID: 'uint32', pcPriClassBase: 'long', dwFlags: 'uint32',
  szExeFile: 'uint16[260]'
});
const CreateToolhelp32Snapshot = kernel32.func('uintptr_t __stdcall CreateToolhelp32Snapshot(uint32_t flags, uint32_t th32ProcessID)');
// The entry is _Inout_, not _Out_: Process32FirstW validates dwSize, so our
// pre-filled size must pass through instead of a zeroed output buffer.
const Process32FirstW = kernel32.func('int __stdcall Process32FirstW(uintptr_t h, _Inout_ PROCESSENTRY32W *entry)');
const Process32NextW = kernel32.func('int __stdcall Process32NextW(uintptr_t h, _Inout_ PROCESSENTRY32W *entry)');

function findProcessIdByName(imageName) {
  const needle = String(imageName || '').toLowerCase();
  if (!needle) return null;
  const h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (!h || h === -1 || h === -1n) return null;
  try {
    const entry = { dwSize: koffi.sizeof(PROCESSENTRY32W) };
    for (let ok = Process32FirstW(h, entry); ok; ok = Process32NextW(h, entry)) {
      const chars = entry.szExeFile;
      let name = '';
      for (let i = 0; i < chars.length && chars[i]; i++) name += String.fromCharCode(chars[i]);
      if (name.toLowerCase() === needle) return entry.th32ProcessID;
    }
  } catch (_) {
    return null;
  } finally {
    try { CloseHandle(h); } catch (_) {}
  }
  return null;
}

// --- window discovery by pid + monitor geometry (Little Remielle pet) ----------
const MonitorProc = koffi.proto('int __stdcall MonitorProc(uintptr_t hMonitor, void *hdc, void *clipRect, void *data)');
const EnumDisplayMonitors = user32.func('int __stdcall EnumDisplayMonitors(void *hdc, void *clipRect, MonitorProc *proc, void *data)');

// Visible top-level windows of a process, largest-area first (the pet's main
// window wins over helper popups). Same two-pass rule as listWindowsByClass:
// collect inside the callback, classify after it returns.
function findPidWindows(pid) {
  const hwnds = [];
  EnumWindows((h) => { hwnds.push(Number(h)); return 1; }, null);
  const out = [];
  for (const hwnd of hwnds) {
    try {
      if (!IsWindowVisible(hwnd)) continue;
      const p = [0];
      GetWindowThreadProcessId(hwnd, p);
      if (Number(p[0]) !== pid) continue;
      const rc = getWindowRect(hwnd);
      if (!rc) continue;
      const area = (rc.right - rc.left) * (rc.bottom - rc.top);
      if (area <= 0) continue;
      out.push({ hwnd, area });
    } catch (_) {}
  }
  out.sort((a, b) => b.area - a.area);
  return out.map((x) => x.hwnd);
}

// One EnumWindows walk that answers both questions the pet guardian asks: the
// pet's main window (largest visible window of `pid`, same rule as
// findPidWindows) and whether it sits BELOW `refHwnd` in the z-order. Folding
// them into one pass keeps the guard tick (~400ms) at the cost of the bar's
// existing z-sync instead of two walks. EnumWindows walks top to bottom, so a
// larger index is further back.
function petGuardSnapshot(pid, refHwnd) {
  const hwnds = [];
  EnumWindows((h) => { hwnds.push(Number(h)); return 1; }, null);
  const ref = Number(refHwnd) || 0;
  let refIdx = -1, pet = 0, petIdx = -1, petArea = 0;
  for (let i = 0; i < hwnds.length; i++) {
    const hwnd = hwnds[i];
    if (hwnd === ref) refIdx = i;
    try {
      if (!IsWindowVisible(hwnd)) continue;
      const p = [0];
      GetWindowThreadProcessId(hwnd, p);
      if (Number(p[0]) !== pid) continue;
      const rc = getWindowRect(hwnd);
      if (!rc) continue;
      const area = (rc.right - rc.left) * (rc.bottom - rc.top);
      if (area <= petArea) continue;
      petArea = area; pet = hwnd; petIdx = i;
    } catch (_) {}
  }
  if (!pet) return { hwnd: 0, topmost: false, belowRef: false };
  return { hwnd: pet, topmost: isTopmost(pet), belowRef: refIdx >= 0 && petIdx > refIdx };
}

function moveWindow(hwnd, x, y) {
  const SWP_NOSIZE = 0x1, SWP_NOZORDER = 0x4, SWP_NOACTIVATE = 0x10;
  try { return !!SetWindowPos(hwnd, 0, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
  catch (_) { return false; }
}

// Physical-pixel rects of every connected monitor. The rect arrives as the
// callback's clipRect pointer and is decoded with koffi.decode inside the
// callback - GetMonitorInfoW after the fact kept failing under koffi, and
// decoding the mapped rect avoids it entirely. Only array pushes and a
// decode happen in the callback body, mirroring the EnumWindows caution.
function getMonitorRects() {
  const out = [];
  try {
    EnumDisplayMonitors(null, null, (hmon, hdc, clip) => {
      try {
        const rc = koffi.decode(clip, 0, 'RECT');
        out.push({ left: rc.left, top: rc.top, right: rc.right, bottom: rc.bottom });
      } catch (_) {}
      return 1;
    }, null);
  } catch (_) {}
  return out;
}

// True when the rect at x,y (w,h, defaults 1x1) intersects any monitor - the
// guard that keeps a saved pet position from landing off-screen or on a
// monitor that is no longer connected.
function rectOnAnyMonitor(x, y, w, h) {
  const mons = getMonitorRects();
  if (!mons.length) return false;
  w = w || 1; h = h || 1;
  return mons.some((m) => x < m.right && x + w > m.left && y < m.bottom && y + h > m.top);
}

module.exports = {
  getClassName, getWindowRect, getFrameBounds, getClientRect, forceSize, isCloaked, listWindowsByClass,
  setWindowPosAfter, isBelowInZOrder, isTopmost, setTopmost, setToolWindow, debugZOrder, raiseAboveTerminalChrome, roundCorners, setCornerPreference, setImmersiveDarkMode, removeBorderColor, hwndNumberFromBuffer, bringToFront,
  isIconic: (h) => !!IsIconic(h), isWindow: (h) => !!IsWindow(h), isVisible: (h) => !!IsWindowVisible(h),
  getBattery, initVolume, getVolume, volumeState, getHwinfoTemp, parseHwinfoCpuTemp,
  findPidWindows, petGuardSnapshot, moveWindow, getMonitorRects, rectOnAnyMonitor, findProcessIdByName,
  getForegroundWindow: () => GetForegroundWindow()
};
