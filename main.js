'use strict';
// WizBar — slim acrylic status bar floating above Windows Terminal + token tracker.
const { app, Tray, Menu, ipcMain, nativeImage, shell, dialog, globalShortcut } = require('electron');
const path = require('path');
const fs = require('fs');
const { ConfigManager, CONFIG_PATH, APP_DIR } = require('./src/config');
const { TerminalTracker } = require('./src/tracker');
const { MetricsEngine } = require('./src/metrics');
const { TokenTracker } = require('./src/tokens');
const { BarWindow } = require('./src/bar');
const native = require('./src/native');

// --- single instance -----------------------------------------------------------
if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    // Second launch = open the token dashboard (handy shortcut: just run wizbar again).
    openDashboard();
  });
}

let configManager, tracker, metrics, tokens, bar, dashWin = null, tray = null;

const DBG = (...a) => {
  if (!configManager || !configManager.config.general.debug) return;
  try { fs.appendFileSync(path.join(APP_DIR, 'debug.log'), new Date().toISOString() + ' ' + a.join(' ') + '\n'); } catch (_) {}
};

function hexToRgb(hex) {
  const h = (hex || '#000').replace('#', '');
  return {
    r: parseInt(h.slice(0, 2), 16) || 0,
    g: parseInt(h.slice(2, 4), 16) || 0,
    b: parseInt(h.slice(4, 6), 16) || 0
  };
}

function themePayload(cfg) {
  const { theme, bar } = cfg;
  const a = (bar.backgroundAlpha ?? 110) / 255; // direct: 0 = clear, 255 = solid tint
  const tint = hexToRgb(bar.backgroundTint || '#FBF2E2');
  return {
    ...theme,
    bar: {
      height: bar.height, fontSize: bar.fontSize, fontFamily: bar.fontFamily,
      align: bar.align, radius: bar.radius, segmentSpacing: bar.segmentSpacing,
      backdrop: bar.backdrop,
      // The page paints the ONLY tint layer (the window is transparent:true),
      // so backgroundAlpha maps 1:1 to real opacity: 0 = invisible, 255 = solid.
      bgCss: bar.backdrop === 'solid'
        ? `rgb(${tint.r},${tint.g},${tint.b})`
        : `rgba(${tint.r},${tint.g},${tint.b},${a.toFixed(3)})`,
      // Opaque version for opaque surfaces (dashboard card follows the bar tint)
      tintOpaque: `rgb(${tint.r},${tint.g},${tint.b})`
    },
    modules: cfg.modules,
    tokens: { showOnBar: cfg.tokens.showOnBar }
  };
}

function statsLoop() {
  setInterval(() => {
    if (!bar || !bar.win) return;
    if (!bar.win.isVisible()) return;
    bar.send('stats', metrics.snapshot());
  }, 100);
}

function raiseDash() {
  if (!dashWin || dashWin.isDestroyed()) return;
  if (dashWin.isMinimized()) dashWin.restore();
  dashWin.show();
  // Act like a normal window being launched: to the front of the normal band
  // AND focused. The synthetic-ALT trick makes Windows grant the foreground
  // switch even though the click came from a non-activated bar window.
  try {
    native.bringToFront(native.hwndNumberFromBuffer(dashWin.getNativeWindowHandle()));
  } catch (_) {}
  dashWin.focus();
}

// NOTE: no z-watchdog. A previous version re-anchored the dashboard above the
// terminal whenever the terminal was foreground — but after a minimize-storm
// the terminal sits DEEP in the z-stack, so that yanked the dashboard to the
// bottom too. The dashboard keeps whatever z it has; summoning it again raises
// it via bringToFront from any position.

function openDashboard() {
  if (dashWin && !dashWin.isDestroyed()) {
    raiseDash();
    return;
  }
  dashWin = new (require('electron').BrowserWindow)({
    width: 840,
    height: 580,
    show: false,
    frame: false,
    // Opaque panel: no see-through ring, no murky tint compositing.
    transparent: false,
    resizable: true,
    backgroundColor: '#FBF2E2',
    // No taskbar button and no Alt-Tab entry — the tray menu and the bar's
    // token chip are the only ways in, and no Electron glyph appears anywhere.
    skipTaskbar: true,
    type: 'toolbar',
    icon: path.join(__dirname, 'assets', 'tray.png'),
    webPreferences: {
      preload: path.join(__dirname, 'renderer', 'dash-preload.js'),
      contextIsolation: true,
      nodeIntegration: false
    }
  });
  dashWin.loadFile(path.join(__dirname, 'renderer', 'dash.html'));
  dashWin.once('ready-to-show', () => {
    raiseDash();
    dashWin.send('tokens', tokens.aggregate());
    dashWin.send('theme', themePayload(configManager.config));
  });
  // ready-to-show can be missed on recreation — never leave the window invisible.
  setTimeout(() => {
    try {
      if (dashWin && !dashWin.isDestroyed() && !dashWin.isVisible()) dashWin.showInactive();
    } catch (_) {}
  }, 1500);
  dashWin.on('closed', () => { dashWin = null; });
}

function buildTray() {
  if (!configManager.config.general.showTray) return;
  const iconPath = path.join(__dirname, 'assets', 'tray.png');
  let img;
  try { img = nativeImage.createFromPath(iconPath); } catch (_) {}
  if (!img || img.isEmpty()) img = nativeImage.createEmpty();
  tray = new Tray(img);
  tray.setToolTip('WizBar');
  const updateMenu = () => {
    tray.setContextMenu(Menu.buildFromTemplate([
      { label: 'Token dashboard', click: () => openDashboard() },
      { type: 'separator' },
      { label: 'Edit config', click: () => shell.openPath(CONFIG_PATH) },
      { label: 'Open config folder', click: () => shell.showItemInFolder(CONFIG_PATH) },
      { label: 'Reload config', click: () => configManager.emit('changed', configManager.config) },
      { type: 'separator' },
      { label: 'Quit WizBar', click: () => { app.quit(); } }
    ]));
  };
  updateMenu();
  // Left-click on the tray icon summons the dashboard too — the tray is the
  // one summon that always works, even when other windows cover the bar chip.
  tray.on('click', () => openDashboard());
  tray.on('double-click', () => openDashboard());
}

function wireBar() {
  bar = new BarWindow(configManager.config);
  bar.create();

  let lastZSync = 0;
  const syncZ = (hwnd, force) => {
    const now = Date.now();
    if (!force && now - lastZSync < 400) return;
    lastZSync = now;
    bar.syncZ(hwnd);
  };

  tracker.on('geometry', (rect) => {
    const bounds = tracker.computeBarBounds(rect, configManager.config.bar);
    if (!bounds) { bar.hide(); return; }
    if (configManager.config.general.debug) DBG('geometry', JSON.stringify(rect), '->', JSON.stringify(bounds));
    bar.applyGeometry(bounds);
    syncZ(tracker.hwnd); // keep bar directly above the terminal's z-position
  });
  tracker.on('visibility', (v) => {
    if (configManager.config.general.debug) DBG('visibility', v);
    if (v) {
      syncZ(tracker.hwnd, true); // re-insert above terminal on restore
    } else {
      bar.hide();
    }
  });
  tracker.on('detached', (h) => { DBG('detached', h); bar.hide(); });
  tracker.on('attached', (h) => {
    DBG('attached', h);
    syncZ(h, true);
    bar.send('theme', themePayload(configManager.config));
  });

  // renderer -> main
  ipcMain.handle('get-config', () => configManager.config);
  ipcMain.handle('get-theme', () => themePayload(configManager.config));
  ipcMain.handle('get-tokens', () => tokens ? tokens.aggregate() : null);
  ipcMain.on('open-dash', () => openDashboard());
  ipcMain.on('bar-context', () => {
    Menu.buildFromTemplate([
      { label: 'Token dashboard', click: () => openDashboard() },
      { type: 'separator' },
      { label: 'Edit config', click: () => shell.openPath(CONFIG_PATH) },
      { label: 'Reload config', click: () => configManager.emit('changed', configManager.config) },
      { type: 'separator' },
      { label: 'Quit WizBar', click: () => app.quit() }
    ]).popup({});
  });
  ipcMain.on('close-dash', () => { if (dashWin) dashWin.close(); });

  tokens.on('updated', (agg) => {
    bar.send('tokens', agg);
    if (dashWin && !dashWin.isDestroyed()) dashWin.send('tokens', agg);
  });

  // Push theme + first stats once renderer is ready
  bar.win.webContents.on('console-message', (_e, level, message, line, sourceId) => {
    if (level >= 2) DBG('bar-console:', message, `(${sourceId}:${line})`);
  });
  bar.win.webContents.on('render-process-gone', (_e, details) => {
    DBG('bar renderer GONE:', JSON.stringify(details));
  });
  bar.win.webContents.on('did-finish-load', () => {
    DBG('bar did-finish-load');
    bar.send('theme', themePayload(configManager.config));
    bar.send('stats', metrics.snapshot());
    bar.send('tokens', tokens.aggregate());
  });

  // config hot reload
  configManager.on('changed', (cfg) => {
    console.log('[wizbar] config reloaded');
    tracker.setConfig(cfg);
    metrics.setConfig(cfg);
    tokens.setConfig(cfg);
    bar.cfg = cfg;
    bar.send('theme', themePayload(cfg));
    bar.send('tokens', tokens.aggregate());
    applyAutostart(cfg.general.autostart);
  });
}

function applyAutostart(enable) {
  // HKCU\...\Run pointing at the silent launcher.
  const { exec } = require('child_process');
  const launcher = path.join(__dirname, 'scripts', 'start-wizbar.vbs');
  const cmd = enable
    ? `reg add "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run" /v WizBar /t REG_SZ /d "${launcher}" /f`
    : `reg delete "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run" /v WizBar /f`;
  exec(cmd, { windowsHide: true }, () => {});
}

app.whenReady().then(() => {
  DBG('whenReady');
  try {
    configManager = new ConfigManager();
    const cfg = configManager.load();
    DBG('config loaded');

    tracker = new TerminalTracker(cfg);
    metrics = new MetricsEngine(cfg);
    tokens = new TokenTracker(cfg);

    wireBar();
    DBG('bar wired');

    buildTray();

    metrics.start();
    tokens.start();
    tracker.start();
    // Ctrl+Alt+D summons/toggles the dashboard from anywhere — works even when
    // the token chip is buried under other windows.
    try {
      globalShortcut.register('Control+Alt+D', () => {
        if (dashWin && !dashWin.isDestroyed()) dashWin.close();
        else openDashboard();
      });
    } catch (_) {}
    DBG('all started');

    statsLoop();
    applyAutostart(cfg.general.autostart);
    DBG('running. config:', CONFIG_PATH);
  } catch (e) {
    DBG('FATAL', e.stack || e.message);
    console.error('FATAL', e);
    dialog.showErrorBox('WizBar failed to start', String(e && e.stack || e));
    app.quit();
  }
});

app.on('will-quit', () => { try { globalShortcut.unregisterAll(); } catch (_) {} });
app.on('window-all-closed', (e) => {
  // Bar/dash closing must not quit the app; only tray Quit does.
});
