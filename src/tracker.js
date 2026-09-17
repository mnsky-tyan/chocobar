'use strict';
// Terminal window tracker.
// Behavior (per spec):
//  - The bar attaches to the FIRST terminal window that appears while it is unattached.
//    (At app start with terminals already open, the frontmost one counts as "first".)
//  - While attached it follows that window only: move, resize (width), minimize (hide), restore (show).
//  - When the followed window closes, the bar detaches and hides. The surviving terminal windows
//    are marked as "old" and do NOT retrigger it; only a NEW terminal window will.
const { screen } = require('electron');
const native = require('./native');

const FOLLOW_INTERVAL_MS = 16;   // 60Hz follow while attached (matches display refresh;
                                 // 120Hz doubled tracker cost for sub-DWM-pixel gains)
const SCAN_INTERVAL_MS = 400;   // look for new terminal windows while unattached

// Window-class tracking is a Win32 capability. On other platforms there is no
// EnumWindows, so the tracker runs in STATIC mode instead: the bar pins to the
// top edge of the primary display's work area and tracks display changes only.
// No polling timer exists in static mode (nothing to follow).
const STATIC_MODE = process.platform !== 'win32';

// Terminal-agnostic target detection. The bar follows whatever terminal the
// user actually runs, not a hard-coded Windows Terminal class.
//
// config.terminal.className overrides everything:
//   - string: probe exactly that Win32 window class (legacy single-class
//     configs keep working untouched)
//   - '' / missing (the default): AUTO mode, candidates probed in this order:
//
//   1. CASCADIA_HOSTING_WINDOW_CLASS  Windows Terminal (stable + Preview)
//   2. ConsoleWindowClass             classic conhost (cmd.exe / powershell.exe)
//   3. VirtualConsoleClass            ConEmu
//   4. mintty                         mintty (Git Bash / Cygwin)
//   5. wezterm-gui.exe                WezTerm     (by owning PROCESS, see below)
//   6. alacritty.exe                  Alacritty   (by owning PROCESS)
//   7. Hyper.exe                      Hyper       (by owning PROCESS)
//
// Target selection (see _listCandidates): the currently FOREGROUND window wins
// when it is a supported terminal, so a console sitting on the desktop never
// shadows the WezTerm the user is actually in. With no supported foreground
// window the lists above are walked in order and the first list with a live,
// visible, real window decides. WezTerm, Alacritty and Hyper are matched by
// process image name instead of window class because they register the
// generic winit/Electron class shared with unrelated apps, so a class probe
// would false-positive.
const AUTO_PROBE_CLASSES = [
  'CASCADIA_HOSTING_WINDOW_CLASS',
  'ConsoleWindowClass',
  'VirtualConsoleClass',
  'mintty'
];
const AUTO_PROBE_PROCESSES = ['wezterm-gui.exe', 'alacritty.exe', 'Hyper.exe'];

// Resolve config.terminal.className into { classes, processes } probe lists.
function resolveProbe(className) {
  if (typeof className === 'string' && className) return { classes: [className], processes: [] };
  return { classes: AUTO_PROBE_CLASSES.slice(), processes: AUTO_PROBE_PROCESSES.slice() };
}

class TerminalTracker extends require('events') {
  constructor(config) {
    super();
    this.probe = resolveProbe(config.terminal.className);
    this.reattachToExisting = !!config.terminal.reattachToExisting;
    this.cfgBar = config.bar;   // bar sizing for static mode
    this.hwnd = null;          // numeric hwnd of followed terminal window
    this.seen = new Set();     // hwnds observed while unattached (only NEW ones retrigger)
    this._timer = null;
    this._hidden = false;
    this._screenListeners = null;
  }

  start() {
    if (STATIC_MODE) return this._startStatic();
    this._ensureScan();
  }

  // --- static mode (non-Windows): top strip of the primary work area ----------
  _startStatic() {
    if (this.hwnd !== null) return; // already static-attached
    this.hwnd = 0;                  // no terminal handle exists; 0 = synthetic
    this.emit('attached', 0);
    this._emitStaticGeometry();
    this._screenListeners = [
      ['display-metrics-changed', () => this._emitStaticGeometry()],
      ['display-removed', () => this._emitStaticGeometry()],
      ['display-added', () => this._emitStaticGeometry()]
    ];
    for (const [ev, fn] of this._screenListeners) screen.on(ev, fn);
  }

  _emitStaticGeometry() {
    if (this.hwnd === null) return; // stopped
    try {
      this.emit('static-geometry', this.computeStaticBarBounds(this.cfgBar || null));
    } catch (_) {}
  }

  // Re-deliver the current static bounds outside the display/config events
  // that normally emit them. The bar window can be rebuilt after an external
  // destroy at any moment, and in static mode the fresh window has no other
  // source for its geometry (no follow loop exists here). No-op on Windows,
  // where the follow loop re-delivers geometry on its own.
  reemitStatic() {
    if (!STATIC_MODE) return;
    this._emitStaticGeometry();
  }

  // Bar bounds pinned to the top of the primary work area, full width.
  computeStaticBarBounds(cfgBar) {
    const bar = cfgBar || { height: 24 };
    const display = screen.getPrimaryDisplay();
    const s = display.scaleFactor || 1;
    const wa = display.workArea;
    return {
      x: wa.x, y: wa.y, width: wa.width, height: bar.height,
      scale: s, mode: 'static-top'
    };
  }

  setCfgBar(config) { this.cfgBar = config.bar; }

  stop() {
    clearInterval(this._timer);
    this._timer = null;
    this.hwnd = null;
    if (this._screenListeners) {
      for (const [ev, fn] of this._screenListeners) {
        try { screen.removeListener(ev, fn); } catch (_) {}
      }
      this._screenListeners = null;
    }
  }

  setConfig(config) {
    const probe = resolveProbe(config.terminal.className);
    const probeChanged = JSON.stringify(probe) !== JSON.stringify(this.probe);
    this.probe = probe;
    this.reattachToExisting = !!config.terminal.reattachToExisting;
    if (STATIC_MODE) { this.setCfgBar(config); this._emitStaticGeometry(); return; }
    if (probeChanged) {
      this.seen.clear();
      this.hwnd = null;
      clearInterval(this._timer);
      this._timer = null;
      this._ensureScan();
    }
  }

  _ensureScan() {
    if (this._timer || this.hwnd) return;
    this._timer = setInterval(() => this._scanTick(), SCAN_INTERVAL_MS);
    this._scanTick();
  }

  // All candidate terminal windows right now, best target first. Every
  // candidate comes from ONE native walk with ONE set of real-window filters,
  // so a process-matched terminal (WezTerm/Alacritty/Hyper) is screened and
  // ordered exactly like a class-matched one - no minimized, cloaked, untitled
  // or offscreen window, and frontmost-before-largest.
  //
  // Ranking: the foreground window wins when it is a supported terminal (the
  // user is in WezTerm while a build console also sits on the desktop; a raw
  // probe-order walk would attach the bar to that console). Otherwise the
  // probe lists are walked in their documented order and the frontmost window
  // of the first yielding list wins.
  _listCandidates() {
    const wins = native.listWindows();            // frontmost first
    const fg = Number(native.getForegroundWindow()) || 0;
    const probeOfClass = new Map();
    this.probe.classes.forEach((cls, i) => probeOfClass.set(cls, i));
    const probeOfPid = new Map();
    const firstProcessProbe = this.probe.classes.length;
    this.probe.processes.forEach((exe, i) => {
      const pid = native.findProcessIdByName(exe);
      if (pid != null) probeOfPid.set(pid, firstProcessProbe + i);
    });
    const buckets = this.probe.classes.concat(this.probe.processes).map(() => []);
    for (const w of wins) {
      const probe = probeOfClass.has(w.cls) ? probeOfClass.get(w.cls) : probeOfPid.get(w.pid);
      if (probe !== undefined && !buckets[probe].includes(w.hwnd)) buckets[probe].push(w.hwnd);
    }
    const ordered = buckets.reduce((all, b) => all.concat(b), []);
    const fgIdx = ordered.indexOf(fg);
    if (fgIdx > 0) { ordered.splice(fgIdx, 1); ordered.unshift(fg); }
    return ordered;
  }

  _scanTick() {
    if (this.hwnd) return;
    const wins = this._listCandidates();
    const fresh = this.reattachToExisting
      ? (wins.length ? [wins[0]] : [])
      : wins.filter((w) => !this.seen.has(w));
    for (const w of wins) this.seen.add(w);
    if (fresh.length) this._attach(fresh[0]);
  }

  _attach(hwnd) {
    this.hwnd = hwnd;
    clearInterval(this._timer);
    this._timer = setInterval(() => this._followTick(), FOLLOW_INTERVAL_MS);
    this._hidden = false;
    this.emit('attached', hwnd);
    this._followTick();
  }

  _detach() {
    if (!this.hwnd) return;
    const wasHwnd = this.hwnd;
    this.hwnd = null;
    clearInterval(this._timer);
    this._timer = null;
    this.emit('detached', wasHwnd);
    // Everything that currently exists is now "old" - only a NEW window retriggers.
    // (Drop the closed hwnd itself so a reused handle value still counts as new.)
    const wins = this._listCandidates();
    this.seen = new Set(wins.filter((w) => w !== wasHwnd));
    this._ensureScan();
  }

  _followTick() {
    const hwnd = this.hwnd;
    if (!hwnd || !native.isWindow(hwnd)) {
      this._detach(); // followed window closed
      return;
    }
    // Visible frame (DWM extended bounds), not GetWindowRect which includes
    // the invisible resize borders and makes the bar wider than the terminal.
    const rect = native.getFrameBounds(hwnd);
    if (!rect) return;

    if (native.isIconic(hwnd) || native.isCloaked(hwnd)) {
      if (!this._hidden) {
        this._hidden = true;
        this.emit('visibility', false);
      }
      return;
    }
    if (this._hidden) {
      this._hidden = false;
      this.emit('visibility', true);
    }
    this.emit('geometry', {
      left: rect.left, top: rect.top, right: rect.right, bottom: rect.bottom
    });
  }

  // Convert a physical-pixel terminal rect into the bar's DIP bounds.
  computeBarBounds(rect, cfgBar) {
    const width = Math.max(1, rect.right - rect.left);
    const height = Math.max(1, rect.bottom - rect.top);
    const display = screen.getDisplayMatching({ x: rect.left, y: rect.top, width, height });
    const s = display.scaleFactor || 1;
    const barH = Math.round(cfgBar.height * s);
    const gap = Math.round(cfgBar.gap * s);
    const inset = Math.round((cfgBar.insetX ?? 0) * s); // shave each side; WT's visible
    const xPhys = rect.left + inset;                    // frame reads a hair wider than
    const wPhys = Math.max(1, width - 2 * inset);       // its body, so trim to match

    const waTopPhys = Math.round(display.workArea.y * s);
    const waBottomPhys = Math.round((display.workArea.y + display.workArea.height) * s);

    let yPhys;
    let mode = 'above';
    if (cfgBar.position === 'below') {
      yPhys = rect.bottom + gap;
      if (yPhys + barH > waBottomPhys) { yPhys = rect.bottom - barH; mode = 'bottom-inside'; }
    } else {
      yPhys = rect.top - barH - gap;
      if (yPhys < waTopPhys) {
        // Not enough room above (maximized / opened at the very top): hide the
        // bar — per spec, don't stuff it somewhere else.
        return null;
      }
    }

    return {
      x: xPhys / s,
      y: yPhys / s,
      width: wPhys / s,
      height: cfgBar.height,
      scale: s,
      mode
    };
  }
}

module.exports = { TerminalTracker, resolveProbe };
