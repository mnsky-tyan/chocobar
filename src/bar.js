'use strict';
// The status bar window: frameless floating strip positioned by the tracker.
// Translucency is real per-pixel alpha (transparent:true + the page's rgba
// tint); corners are the page's CSS border-radius. See create() for why
// DWM backgroundMaterial/rounding/shadow are not used.
const path = require('path');
const { BrowserWindow } = require('electron');
const native = require('./native');

class BarWindow {
  constructor(config) {
    this.cfg = config;
    this.win = null;
    this.hwnd = null;          // numeric hwnd of the bar itself (for z-order sync)
    this._lastBoundsKey = '';
    this._staticContent = false; // last applied staticWidth mode was 'content'
  }

  create() {
    if (this.win) return this.win;
    const bar = this.cfg.bar;

    // transparent:true is what actually delivers user-controlled opacity: the
    // page paints rgba(tint, alpha/255) and the OS composites it for real —
    // alpha 0 = invisible, 255 = solid. DWM backgroundMaterial composites the
    // window OPAQUELY (alpha byte ignored) and was why the bar never went
    // see-through no matter how backgroundAlpha was set.
    this.win = new BrowserWindow({
      width: 900,
      height: bar.height,
      show: false,
      frame: false,
      transparent: true,
      resizable: false,
      minimizable: false,
      maximizable: false,
      skipTaskbar: true,
      // The bar never belongs in the taskbar. If an entry ever resurrects for it
      // (display power events re-showing the window after long uptime have done
      // this), it must at least carry the WizBar icon, never the Electron glyph.
      icon: path.join(__dirname, '..', 'assets', 'tray.png'),
      // focusable must stay DEFAULT (true): Electron combines focusable:false
      // with transparent:true into a click-through window on Windows (the OS
      // hit-tests it as transparent, so real mouse clicks never reach the
      // chip/hotkeys). The bar doesn't steal focus when clicked unless raised.
      hasShadow: false,
      alwaysOnTop: false,
      backgroundColor: '#00000000',
      webPreferences: {
        preload: path.join(__dirname, '..', 'renderer', 'bar-preload.js'),
        contextIsolation: true,
        nodeIntegration: false,
        backgroundThrottling: true
      }
    });

    this.hwnd = native.hwndNumberFromBuffer(this.win.getNativeWindowHandle());
    // Structural taskbar exclusion: WS_EX_TOOLWINDOW makes the shell skip the
    // window on EVERY enumeration, so no taskbar rebuild (explorer restart,
    // sleep/wake, display topology change) can ever resurrect a button for the
    // bar. Applied while the window is still unshown. Alt-Tab exclusion is a
    // welcome side effect - the bar is a passive strip, never a switch target.
    native.setToolWindow(this.hwnd, true);
    // Never activate on click: activation raises the bar above every app
    // (a flicker over foreground apps) and steals foreground from the
    // terminal the click serves. Mouse events still deliver to the page.
    native.setNoActivate(this.hwnd);
    // And never rise on click either (WM_MOUSEACTIVATE -> MA_NOACTIVATE).
    native.noActivateProc(this.hwnd);

    this.win.loadFile(path.join(__dirname, '..', 'renderer', 'bar.html'));

    // An externally destroyed window (Alt-F4, shell close) must not leave this
    // reference dangling on a destroyed object - the stats and geometry loops
    // poll it every tick and would throw "Object has been destroyed" forever.
    this.win.on('closed', () => {
      this.win = null;
      this._shouldShow = false;
      // The heal interval must not outlive its window: main.js rebuilds the bar
      // as a NEW BarWindow on 'closed', and without this each rebuild would
      // leak another 400ms timer doing no-op work forever.
      if (this._healTimer) { clearInterval(this._healTimer); this._healTimer = null; }
    });

    // Periodic size guard (see healSize) — heals any OS-side growth of the window.
    this._sizeTarget = null;
    this._healTimer = setInterval(() => {
      if (this._sizeTarget) this.healSize(this._sizeTarget.w, this._sizeTarget.h, this._sizeTarget.scale);
      this.assertNoTaskbar(); // keep the shell from ever minting a taskbar button
    }, 400);
    return this.win;
  }

  // All bar visuals (tint, alpha, backdrop) are painted by the renderer and
  // arrive via the theme push — there is no native color layer to re-apply.

  applyGeometry(bounds, staticWidth) {
    if (!this.win || this.win.isDestroyed()) return;
    this._shouldShow = true; // bounds exist → the bar belongs on screen
    const content = staticWidth === 'content';
    const key = `${bounds.x.toFixed(1)},${bounds.y.toFixed(1)},${bounds.width.toFixed(1)},${bounds.height}`;
    this._sizeTarget = { w: Math.round(bounds.width), h: Math.round(bounds.height), scale: bounds.scale || 1 };
    // Unchanged bounds re-applies (config saves, spurious display events) must
    // touch nothing: in pill mode the shrunk window IS the correct size, and
    // re-expanding would flash a full-width click-swallowing strip until the
    // next renderer report. Exception: leaving pill mode — the window may
    // still sit at the shrunk pill size while the bounds already say full
    // strip (staticWidth hot-reload re-emits identical geometry), so re-apply
    // to expand it back. _staticContent records the last SEEN mode (updated
    // before this gate): a mode flip with identical bounds must still register,
    // or the return flip to 'workarea' would not re-expand the shrunk window.
    const leavingPill = this._staticContent && !content;
    this._staticContent = content;
    if (key === this._lastBoundsKey && !leavingPill) return;
    this._lastBoundsKey = key;
    this.win.setBounds({
      x: Math.round(bounds.x), y: Math.round(bounds.y),
      width: Math.round(bounds.width), height: Math.round(bounds.height)
    });
    // Guard against any platform height fudging — the renderer's flex layout
    // centers on the content size, so it must equal the configured bar height.
    const [, ch] = this.win.getContentSize();
    if (Math.abs(ch - Math.round(bounds.height)) > 1) {
      this.win.setContentSize(Math.round(bounds.width), Math.round(bounds.height));
    }
    if (!this.win.isVisible()) {
      this.win.showInactive();
      // Same as the heal re-show: a show lands the bar above every app; the
      // terminal-layer insertion must follow immediately.
      this.onShown && this.onShown();
    }
    this.assertNoTaskbar();
    // The window was just resized out of pill mode (static-geometry re-apply,
    // display change, staticWidth hot-reload): forget the last reported pill
    // width so the next 'bar-content-size' report re-shrinks the window.
    this._lastPillW = 0;
  }

  // Static corner-pill mode: shrink to the renderer-reported content width,
  // anchored top-right of the work area (an invisible wider strip would
  // swallow clicks along the top edge). Only reachable while the static
  // width mode is 'content'; applyGeometry expands the window again when
  // the mode leaves 'content' (a staticWidth hot-reload re-emits identical
  // geometry, so the transition cannot be seen from the bounds alone).
  setPillWidth(width, barCfg) {
    if (!this.win || this.win.isDestroyed()) return;
    this._lastPillW = width;
    try {
      const { screen } = require('electron');
      const wa = screen.getPrimaryDisplay().workArea;
      const margin = 6;
      this.win.setBounds({
        x: wa.x + wa.width - width - margin,
        y: wa.y + margin,
        width,
        height: barCfg.height
      });
    } catch (_) {}
  }

  // Both layers of taskbar exclusion, re-asserted. setSkipTaskbar is Electron's
  // ITaskbarList::DeleteTab: stateless, and forgotten the moment the shell
  // re-enumerates windows - which is exactly why PR#5's per-show re-assert
  // still let the icon return after long uptime. setToolWindow is the
  // structural half (WS_EX_TOOLWINDOW re-read on every enumeration); each
  // call is a cheap no-op unless something cleared the state.
  // A 2s floor keeps the periodic re-assert from turning into constant shell
  // COM chatter (DeleteTab is not free); anything that genuinely clears the
  // state is re-asserted within 2s, which no shell rebuild outlasts.
  assertNoTaskbar() {
    if (!this.win || this.win.isDestroyed() || !this.hwnd) return;
    const now = Date.now();
    if (this._taskbarAssertedAt && now - this._taskbarAssertedAt < 2000) return;
    this._taskbarAssertedAt = now;
    native.setToolWindow(this.hwnd, true);
    native.setNoActivate(this.hwnd); // re-asserted: anything may rewrite ex-styles
    this.win.setSkipTaskbar(true);
  }

  // Keep the OS window at the exact physical size Electron believes. Windows
  // sometimes grows frameless windows by an invisible band after show; that band
  // renders as a raw grey acrylic strip along the bar's bottom edge.
  healSize(targetW, targetH, scale) {
    if (!this.win || this.win.isDestroyed() || !this.hwnd) return;
    if (native.inMoveSize()) return; // never fight a live drag
    // Self-heal visibility — but ONLY when the bar belongs on screen. Without
    // this guard the heal loop fights hide() on minimize/detach (flicker).
    let shown = false;
    if (!this.win.isVisible() && this._shouldShow && this._sizeTarget) {
      console.log('[wizbar] heal: window was hidden, re-showing');
      this.win.showInactive();
      shown = true;
    }
    this.assertNoTaskbar();
    const wPhys = Math.round(targetW * (scale || 1));
    const hPhys = Math.round(targetH * (scale || 1));
    // An environment that keeps re-breaking the size makes this fire every
    // tick; log at most one line per 5 minutes (the correction still
    // applies every tick).
    const healLog = (msg) => {
      if (Date.now() - (this._lastHealLogAt || 0) < 300000) return;
      this._lastHealLogAt = Date.now();
      console.log('[wizbar] heal:', msg);
    };
    const [, ch] = this.win.getContentSize();
    if (Math.abs(ch - targetH) > 1) {
      healLog('content ' + ch + ' -> ' + targetH);
      this.win.setContentSize(Math.round(targetW), targetH);
    }
    const rc = native.getWindowRect(this.hwnd);
    if (rc && (Math.abs(rc.right - rc.left - wPhys) > 1 || Math.abs(rc.bottom - rc.top - hPhys) > 1)) {
      healLog('phys ' + (rc.right - rc.left) + 'x' + (rc.bottom - rc.top) + ' -> ' + wPhys + 'x' + hPhys);
      native.forceSize(this.hwnd, wPhys, hPhys);
    }
    // A show lands the window at the top of its z-band — above every other
    // app. The bar belongs ON the followed terminal's layer (inserted directly
    // above it), so re-assert the z-insertion right after any show; with the
    // geometry dedup the next regular syncZ could be a long wait.
    if (shown) this.onShown && this.onShown();
  }

  // Keep the bar hovering immediately above the followed terminal in z-order
  // (same band as the terminal: apps above the terminal cover the bar too).
  // Must also clear the terminal's invisible DRAG_BAR_WINDOW_CLASS overlay,
  // which would otherwise eat every mouse click aimed at the bar's strip.
  syncZ(terminalHwnd) {
    if (this.hwnd && terminalHwnd) native.raiseAboveTerminalChrome(this.hwnd, terminalHwnd);
  }

  hide() {
    this._shouldShow = false; // healSize must NOT resurrect a deliberate hide
    if (this.win && this.win.isVisible()) this.win.hide();
    this._lastBoundsKey = '';
    this._taskbarAssertedAt = 0; // next show re-asserts immediately
    this._lastPillW = 0;
  }

  send(channel, payload) {
    if (this.win && !this.win.isDestroyed()) {
      this.win.webContents.send(channel, payload);
    }
  }

  destroy() {
    if (this._healTimer) { clearInterval(this._healTimer); this._healTimer = null; }
    if (this.win && !this.win.isDestroyed()) this.win.destroy();
    this.win = null;
    this.hwnd = null;
    this._lastPillW = 0;
  }
}

module.exports = { BarWindow };
