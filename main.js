'use strict';
// WizBar — slim acrylic status bar floating above Windows Terminal + token tracker.
const { app, Tray, Menu, ipcMain, nativeImage, shell, dialog, globalShortcut } = require('electron');
const path = require('path');
const fs = require('fs');
const { spawn, execFile } = require('child_process');
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
  let lastVals = '';
  setInterval(() => {
    if (!bar || !bar.win || bar.win.isDestroyed()) return;
    if (!bar.win.isVisible()) return;
    // Push only when a module value actually changed: every snapshot() stamps
    // `now`, so at a fixed 100ms tick every push looked new — 10 IPC messages
    // and full bar re-renders per second for values moving at 0.5-5s cadences.
    const snap = metrics.snapshot();
    const { now, ...vals } = snap;
    const sig = JSON.stringify(vals);
    if (sig === lastVals) return;
    lastVals = sig;
    bar.send('stats', snap);
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

// No z-watchdog for the dashboard, on purpose. The dash is a NORMAL window
// (see openDashboard): when the windows above it close or minimize, Windows
// activates it instead of skipping it, so it can't be demoted the way the old
// toolwindow dash was. Re-raising from a watchdog would also fight deliberate
// clicks — a user who clicks the terminal wants the terminal in front.

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
    // A NORMAL window, deliberately. The old toolwindow styling (skipTaskbar +
    // type:'toolbar') made Windows skip the dash when choosing the next window
    // to activate — so the moment every window above it closed/minimized,
    // activation fell through to the terminal and raised it over the dash
    // (the "dashboard sinks" bug). As a normal window the dash simply gets
    // activated when it becomes the top window and can never be demoted that
    // way. Cost: a taskbar/Alt-Tab entry while open (with the WizBar icon —
    // no Electron glyph); the tray stays empty either way.
    skipTaskbar: false,
    icon: path.join(__dirname, 'assets', 'tray.png'),
    title: 'WizBar dashboard',
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
  for (const ev of ['minimize', 'restore', 'show', 'hide', 'maximize', 'unmaximize']) {
    dashWin.on(ev, () => DBG('dash event:', ev));
  }
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

// --- Little Remielle desktop-pet toggle ------------------------------------------
// The bow chip on the bar: click = launch the exe (detached), click again =
// kill it. Truth comes from a process-name poll (not the child handle) so a pet
// started or stopped outside WizBar is reflected too; the in-process Toolhelp
// snapshot reads proper UTF-16 image names, so no console-codepage workarounds.
let remielleState = { running: false, exists: false };
let remiellePid = null;          // pet process id from the last live poll
let remielleRestorePending = false; // set on launch, consumed by the poll
let remielleRestoreActive = false;  // true while the restore watcher runs
let remielleDefaultSig = null;   // the pet's built-in default spot (x,y),
                                 // learned during restore - never save it,
                                 // or a poll would clobber the real
                                 // position with the default every time
let remielleLastPosSig = null;   // skip re-writing an unchanged position

function remiellePosFile() { return path.join(APP_DIR, 'remielle-position.json'); }

 // The pet keeps its own position memory in 设置.json next to its exe and
 // restores it on every launch - that file, not WizBar, was what put the
 // figure back at the wrong spot on every toggle. Make WizBar the authority:
 // before launching, write the saved position into the pet’s own settings so
 // the pet places itself where the captain left it. Coordinates are physical
 // pixels, same space as GetWindowRect; the sprite scale field is preserved.
function remielleSyncPetConfig(x, y) {
  const cfg = remielleCfg();
  if (!cfg.exePath) return;
  const petFile = path.join(path.dirname(cfg.exePath), '设置.json');
  try {
    let scale = 1;
    try { scale = JSON.parse(fs.readFileSync(petFile, 'utf8')).scale || 1; } catch (_) {}
    fs.writeFileSync(petFile, JSON.stringify({ x, y, scale }), 'utf8');
  } catch (e) { DBG('remielle pet-config sync failed:', e.message); }
}

// Remember where the figure is: its window rect, written while it runs and
// right before we kill it, so the next toggle and the next app restart can
// put it back exactly where the captain left it.
function remielleSavePosition() {
  if (!remiellePid) return;
  // A poll must never record the pet’s transient states: while a restore is
  // pending/running the window is being placed by us, and the built-in default
  // spot is exactly what the restore exists to move it away from. Saving
  // either would clobber the captain’s position and poison every later
  // toggle (the works-once-or-twice-then-fails bug).
  if (remielleRestorePending || remielleRestoreActive) return;
  try {
    const hwnds = native.findPidWindows(remiellePid);
    if (!hwnds.length) return;
    const rc = native.getWindowRect(hwnds[0]);
    if (!rc || rc.right <= rc.left || rc.bottom <= rc.top) return;
    const sig = rc.left + ',' + rc.top;
    if (sig === remielleLastPosSig) return;
    if (remielleDefaultSig && sig === remielleDefaultSig) return; // the default is not a placement
    remielleLastPosSig = sig;
    fs.writeFileSync(remiellePosFile(), JSON.stringify({
      x: rc.left, y: rc.top, w: rc.right - rc.left, h: rc.bottom - rc.top, savedAt: new Date().toISOString()
    }));
  } catch (e) { DBG('remielle position save failed:', e.message); }
}

// Put the figure back where it was. A saved position that lands off-screen
// or on a monitor that is no longer connected is ignored: the pet then shows
// up at its own default position instead.
function remielleRestorePosition(pid) {
  let pos = null;
  try { pos = JSON.parse(fs.readFileSync(remiellePosFile(), 'utf8')); } catch (_) { return; }
  if (!pos || typeof pos.x !== 'number' || typeof pos.y !== 'number') return;
  if (!native.rectOnAnyMonitor(pos.x, pos.y, pos.w, pos.h)) {
    DBG('remielle restore: saved position is off-screen; keeping the default');
    return;
  }
  let tries = 0, moved = false, reasserts = 0, startedAt = 0;
  const finish = () => { remielleRestoreActive = false; };
  remielleRestoreActive = true;
  const iv = setInterval(() => {
    const hwnds = native.findPidWindows(pid);
    if (!hwnds.length) {
      if (moved) { clearInterval(iv); finish(); return; } // pet closed; done
      if (++tries > 20) { clearInterval(iv); finish(); DBG('remielle restore: window never appeared'); }
      return;
    }
    const rc = native.getWindowRect(hwnds[0]);
    if (!rc) return;
    const sig = rc.left + ',' + rc.top;
    if (!moved) {
      // where the pet placed itself is its built-in default: remember it so
      // poll-time saves can never record it (that clobbered the captain's
      // position and broke every toggle after the first couple)
      remielleDefaultSig = sig;
      native.moveWindow(hwnds[0], pos.x, pos.y);
      moved = true; startedAt = Date.now();
      DBG('remielle restore: moved pet to', pos.x, pos.y);
      return;
    }
    // The pet’s own startup init can re-snap the window to its default after
    // our move; re-assert for a bounded window. The moment the rect is
    // neither the default nor the saved spot, the captain is dragging it -
    // stand down immediately and never fight the captain.
    if (sig === remielleDefaultSig && reasserts < 30) {
      native.moveWindow(hwnds[0], pos.x, pos.y);
      reasserts++;
      return;
    }
    if (sig !== pos.x + ',' + pos.y || Date.now() - startedAt > 20000) { clearInterval(iv); finish(); }
  }, 500);
}

function remielleCfg() {
  return (configManager && configManager.config.modules || {}).remielle || {};
}

function pushRemielle() {
  if (bar && bar.win && !bar.win.isDestroyed()) bar.send('remielle', remielleState);
}

function pollRemielle() {
  const cfg = remielleCfg();
  if (!cfg.enabled) return;
  const exe = cfg.exePath;
  if (!exe || !fs.existsSync(exe)) {
    remielleState = { running: false, exists: false };
    pushRemielle();
    return;
  }
  // In-process lookup — spawning tasklist.exe cost ~290ms of CPU per poll,
  // 20 polls per minute, just to watch one process.
  const pid = native.findProcessIdByName(path.basename(exe));
  const running = pid != null;
  remiellePid = running ? pid : null;
  if (running) remielleSavePosition();
  if (running && remielleRestorePending && pid) { remielleRestorePending = false; remielleRestorePosition(pid); }
  if (remielleState.exists !== true || remielleState.running !== running) {
    DBG('remielle poll:', JSON.stringify({ running, pid }));
  }
  remielleState = { running, exists: true };
  pushRemielle();
}

function toggleRemielle() {
  const cfg = remielleCfg();
  const exe = cfg.exePath;
  if (!cfg.enabled || !exe || !fs.existsSync(exe)) return;
  if (remielleState.running) {
    remielleSavePosition(); // capture the last spot before the process dies
    execFile('taskkill', ['/IM', path.basename(exe), '/F', '/T'], { windowsHide: true },
      () => setTimeout(pollRemielle, 300));
  } else {
    // Position authority: seed the pet’s own settings with the saved spot so
    // it launches there (its init would otherwise put it back at whatever its
    // own settings file held).
    let saved = null;
    try { saved = JSON.parse(fs.readFileSync(remiellePosFile(), 'utf8')); } catch (_) {}
    if (saved && typeof saved.x === 'number' && typeof saved.y === 'number' && native.rectOnAnyMonitor(saved.x, saved.y, saved.w, saved.h)) {
      remielleSyncPetConfig(saved.x, saved.y);
    }
    try {
      spawn(exe, [], { cwd: path.dirname(exe), detached: true, stdio: 'ignore' }).unref();
    } catch (e) { DBG('remielle spawn failed:', e.message); }
    remielleRestorePending = true; // the poll hands the pid to the restore flow
    setTimeout(pollRemielle, 1500);
  }
}

function spawnBar() {
  bar = new BarWindow(configManager.config);
  bar.create();
  // Self-heal: if the bar window is destroyed externally (Alt-F4, shell
  // close), rebuild it instead of leaving the stats and geometry loops
  // throwing on a dangling window. Skipped while the app is quitting.
  bar.win.on('closed', () => {
    setTimeout(() => {
      if (!shuttingDown && bar && (!bar.win || bar.win.isDestroyed())) spawnBar();
    }, 250);
  });
}

let shuttingDown = false;

function wireBar() {
  spawnBar();

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
  ipcMain.handle('rescan-tokens', () => tokens ? tokens.rescan() : null);
  ipcMain.handle('toggle-remielle', () => { toggleRemielle(); return remielleState; });
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
    bar.send('tokens', { today: agg.today }); // bar reads today only; full agg goes to the dash
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
    bar.send('tokens', { today: tokens.aggregate().today }); // bar reads today only; full agg goes to the dash
  });

  // config hot reload
  configManager.on('changed', (cfg) => {
    console.log('[wizbar] config reloaded');
    tracker.setConfig(cfg);
    metrics.setConfig(cfg);
    tokens.setConfig(cfg);
    bar.cfg = cfg;
    bar.send('theme', themePayload(cfg));
    bar.send('tokens', { today: tokens.aggregate().today }); // bar reads today only; full agg goes to the dash
    applyAutostart(cfg.general.autostart);
    // Tray follows showTray live (no restart needed to add/remove it).
    if (tray && !cfg.general.showTray) { tray.destroy(); tray = null; }
    else if (!tray && cfg.general.showTray) buildTray();
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
    pollRemielle();
    setInterval(pollRemielle, 3000);
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

app.on('will-quit', () => {
  remielleSavePosition(); // the pet survives the quit; remember where it sits
  try { globalShortcut.unregisterAll(); } catch (_) {}
});
app.on('window-all-closed', (e) => {
  // Bar/dash closing must not quit the app; only tray Quit does.
});
