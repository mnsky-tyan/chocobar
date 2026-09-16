'use strict';
// WizBar config: defaults merged with %USERPROFILE%\.wizbar\config.json, hot-reloaded on change.
const fs = require('fs');
const path = require('path');
const os = require('os');
const { EventEmitter } = require('events');

const APP_DIR = path.join(os.homedir(), '.wizbar');
const CONFIG_PATH = path.join(APP_DIR, 'config.json');

// Strip // comments (the config file is documented inline), respecting strings.
function stripJsonComments(text) {
  let out = '';
  let inStr = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (inStr) {
      out += c;
      if (c === '\\') { out += text[i + 1] || ''; i++; }
      else if (c === '"') inStr = false;
      continue;
    }
    if (c === '"') { inStr = true; out += c; continue; }
    if (c === '/' && text[i + 1] === '/') {
      while (i < text.length && text[i] !== '\n') i++;
      continue;
    }
    out += c;
  }
  return out;
}

const DEFAULTS = {
  bar: {
    height: 24,            // DIP, slim
    gap: 16,               // DIP of air between the bar and the terminal's top edge
    radius: 8,             // corner rounding, painted by the page (CSS border-radius)
    insetX: 2,             // DIP shaved per side so the bar doesn't overhang the terminal frame
    fontSize: 11,
    fontFamily: "'MesloLGLDZ Nerd Font', 'Cascadia Mono', Consolas, monospace",
    align: 'right',        // right | left | center
    position: 'above',     // above | below
    // Non-Windows only, static bar: 'content' (default) = small pill hugging
    // the chips at the top-right corner, out of the way of everything;
    // 'workarea' = full-width strip pinned to the top of the screen (matches
    // the bar above a maximized terminal, but covers that 24px band).
    staticWidth: 'content',
    backdrop: 'acrylic',   // translucent tint over the desktop | 'solid' = opaque
    backgroundTint: '#FBF2E2',   // pale yellow/pink blend to match terminal acrylic
    backgroundAlpha: 110,        // 0-255 — fill opacity (0 = clear, 255 = solid)
    segmentSpacing: 14
  },
  theme: {
    fg: '#080808',
    fgDim: '#5a5245',
    pink: '#F0DEE4',      // paler rose (was #E8C7D0)
    pinkDeep: '#D493AA',  // softened accent (was #C77B96)
    pinkBg: '#FEF7F9',    // airier blush dashboard surface (was #FDEFF2)
    yellow: '#B8A96A',
    yellowBg: '#F5F0D8',
    warn: '#A00000',
    good: '#006400',
    divider: '#D9CCB2'
  },
  modules: {
    gpu:      { enabled: true, mode: 'sum', intervalMs: 1200 },
    cpu:      { enabled: true, intervalMs: 800, warnAt: 85 },
    cputemp:  { enabled: true, intervalMs: 2000, warnAt: 85 },  // HWiNFO shm (Windows), sysfs sensors elsewhere
    ram:      { enabled: true, intervalMs: 800, warnAt: 90 },
    volume:   { enabled: true, intervalMs: 500, role: 'multimedia' },
    battery:  { enabled: true, intervalMs: 1500 },
    bluetooth:{ enabled: true, intervalMs: 30000, filter: '', maxDevices: 2, hideWhenEmpty: false },
    // Desktop-pet toggle chip. WINDOWS-ONLY, private/local module, OFF in the
    // public build: set enabled + exePath in your own config to use it.
    pet:      { enabled: false, exePath: '' },
    clock:    { enabled: true, format: '{MMM} {dd}  {HH}:{mm}' }
  },
  terminal: {
    // Win32 window class to follow. '' (default) = auto-detect the terminal:
    // Windows Terminal, classic conhost, ConEmu, mintty by window class, then
    // WezTerm / Alacritty / Hyper by owning process (see src/tracker.js for
    // the probe order). A string pins one class; an array is a custom order.
    className: '',
    reattachToExisting: false   // after followed window closes, wait for a NEW window
  },
  tokens: {
    enabled: false,        // master switch: off = no scans, no dashboard data, no chip
    showOnBar: true,
    rescanMinutes: 1,
    heatmapWeeks: 26,
    // Usage sources — all opt-in, all local read-only. Enable the ones you use
    // and point them at your own stores; with none enabled the bar and the
    // dashboard stay empty.
    sources: {
      zcode:    { enabled: false, dbPath: '' },   // e.g. ~/.zcode/cli/db/db.sqlite
      zai:      { enabled: false, sessionsDir: '' }, // e.g. ~/.zai/agent/sessions
      pi:       { enabled: false, sessionsDir: '' }, // e.g. ~/.pi/agent/sessions
      opencode: { enabled: false, storageDir: '' },   // e.g. ~/.local/share/opencode/storage/message
      // Xiaomi MiMo AI desktop: reads the app's local HTTP API while it runs
      // (port + token auto-discovered from the app's own desktop-api.json).
      mimo:     { enabled: false },
      // Subscription plan usage: reads a small JSON file you keep anywhere
      // (e.g. exported from your provider's plan page):
      //   { "plans": [ { "name": "Pro Plan", "total": 1500, "used": 430,
      //                  "resetsAt": "2026-10-14" } ] }
      // and shows each plan's credit usage on the dashboard.
      subscription: { enabled: false, usagePath: '' }
    }
  },
  general: {
    showTray: false,
    autostart: false,
    debug: false
  }
};

// The config file users edit — same as DEFAULTS but with inline explanations.
// Save the file and the bar hot-reloads within a second; no restart needed.
const TEMPLATE = `// Chocobar config — edit any value and save; changes apply live.
// Colors are CSS hex (#RRGGBB). Sizes are DIP (CSS pixels).
{
  "bar": {
    // height of the bar strip
    "height": 24,
    // air between the bar and the terminal's top edge (the floating gap)
    "gap": 16,
    // trim per side so the bar doesn't overhang the terminal frame (0 to disable)
    "insetX": 2,
    // corner rounding of the bar box (the page paints it; no window shadow)
    "radius": 8,
    // text size + font (must be an installed font)
    "fontSize": 11,
    "fontFamily": "'MesloLGLDZ Nerd Font', 'Cascadia Mono', Consolas, monospace",
    // where the modules sit: right | left | center
    "align": "right",
    // bar above or below the terminal window
    "position": "above",
    // non-Windows only: "content" = corner pill (default, out of the way);
    // "workarea" = full-width strip at the top of the screen (like above a
    // maximized terminal)
    "staticWidth": "content",
    // "acrylic" = translucent tint over the desktop, "solid" = opaque
    "backdrop": "acrylic",
    // the tint color of the box
    "backgroundTint": "#FBF2E2",
    // box fill opacity: 0 = fully clear, 255 = solid color.
    // lower = more transparent. ~110 is airy, ~180 is creamy.
    "backgroundAlpha": 110,
    // spacing between the modules (tokens, cpu, ...)
    "segmentSpacing": 14
  },
  "theme": {
    "fg": "#080808",
    "fgDim": "#5a5245",
    "pink": "#F0DEE4",
    "pinkDeep": "#D493AA",
    "pinkBg": "#FEF7F9",
    "yellow": "#B8A96A",
    "yellowBg": "#F5F0D8",
    "warn": "#A00000",
    "good": "#006400",
    "divider": "#D9CCB2"
  },
  "modules": {
    // GPU %: mode "sum" adds all engines (Task-Manager-like), "max" takes the busiest
    "gpu":       { "enabled": true, "mode": "sum", "intervalMs": 1200 },
    "cpu":       { "enabled": true, "intervalMs": 800, "warnAt": 85 },
    // CPU temperature: Windows reads HWiNFO's shared memory (enable "Shared Memory
    // Support" in HWiNFO's settings); Linux reads sysfs sensors (hwmon / thermal
    // zones). Shows "—" when no sensor is publishing.
    "cputemp":   { "enabled": true, "intervalMs": 2000, "warnAt": 85 },
    "ram":       { "enabled": true, "intervalMs": 800, "warnAt": 90 },
    "volume":    { "enabled": true, "intervalMs": 500, "role": "multimedia" },
    "battery":   { "enabled": true, "intervalMs": 1500 },
    // earbud/BT battery; shows "—" unless a device reports via Windows' standard
    // battery property (many earbuds only report to their vendor app)
    "bluetooth": { "enabled": true, "intervalMs": 30000, "filter": "", "maxDevices": 2 },
    // Desktop-pet toggle chip (WINDOWS-ONLY, private module, off by default).
    // Set enabled + exePath here to show the bow chip: click launches the exe,
    // click again stops it ("on"/"off").
    "pet":       { "enabled": false, "exePath": "" },
    // {MMM} month, {dd} day, {HH} {mm} {ss} time (24h)
    "clock":     { "enabled": true, "format": "{MMM} {dd}  {HH}:{mm}" }
  },
  "terminal": {
    // which terminal to follow. "" (default) auto-detects: Windows Terminal,
    // classic conhost (cmd/PowerShell console), ConEmu and mintty by window
    // class, then WezTerm / Alacritty / Hyper by process. Set a Win32 class
    // here to pin one specific window class instead.
    "className": "",
    // false: after the followed window closes, only a NEW terminal re-triggers the bar.
    // true: the bar grabs whatever terminal already exists instead.
    "reattachToExisting": false
  },
  "tokens": {
    // master switch: off = no scans, no dashboard data, no usage chip;
    // on = the per-source flags below decide which stores are read
    "enabled": false,
    "showOnBar": true,
    // minutes between usage scans (drives the dashboard's live refresh)
    "rescanMinutes": 1,
    "heatmapWeeks": 26,
    // usage sources — all opt-in, all local read-only. Enable the ones you use
    // and point them at your own stores; the typical locations are shown in
    // the comments. With none enabled, bar + dashboard stay empty.
    "sources": {
      "zcode":    { "enabled": false, "dbPath": "" },       // e.g. "~/.zcode/cli/db/db.sqlite"
      "zai":      { "enabled": false, "sessionsDir": "" },  // e.g. "~/.zai/agent/sessions"
      "pi":       { "enabled": false, "sessionsDir": "" },  // e.g. "~/.pi/agent/sessions"
      "opencode": { "enabled": false, "storageDir": "" },   // e.g. "~/.local/share/opencode/storage/message"
      // Xiaomi MiMo AI desktop app token usage, read live from its local API
      // while the app runs (auto-discovered; nothing to configure).
      "mimo":     { "enabled": false },
      // subscription plan credit usage, read from a JSON file you point at:
      //   { "plans": [ { "name": "Pro Plan", "total": 1500, "used": 430,
      //                  "resetsAt": "2026-10-14" } ] }
      "subscription": { "enabled": false, "usagePath": "" }   // e.g. "~/.chocobar/plan-usage.json"
    }
  },
  "general": {
    "showTray": false,
    // start Chocobar at login (writes an HKCU Run entry)
    "autostart": false,
    "debug": false
  }
}
`;

function expandTilde(p) {
  if (typeof p === 'string' && p.startsWith('~/')) {
    return path.join(os.homedir(), p.slice(2));
  }
  return p;
}

function deepMerge(base, over) {
  const out = Array.isArray(base) ? base.slice() : { ...base };
  if (!over || typeof over !== 'object') return out;
  for (const [k, v] of Object.entries(over)) {
    if (v && typeof v === 'object' && !Array.isArray(v) && base && typeof base[k] === 'object' && !Array.isArray(base[k])) {
      out[k] = deepMerge(base[k], v);
    } else if (v !== undefined) {
      out[k] = v;
    }
  }
  return out;
}

class ConfigManager extends EventEmitter {
  constructor() {
    super();
    this.config = null;
    this._watcher = null;
    this._debounce = null;
  }

  load() {
    if (!fs.existsSync(APP_DIR)) fs.mkdirSync(APP_DIR, { recursive: true });
    let user = {};
    if (fs.existsSync(CONFIG_PATH)) {
      try {
        user = JSON.parse(stripJsonComments(fs.readFileSync(CONFIG_PATH, 'utf8')));
      } catch (e) {
        console.error('[wizbar] config parse error, using defaults:', e.message);
      }
    } else {
      // First run: write the annotated template out so the user can customize.
      try { fs.writeFileSync(CONFIG_PATH, TEMPLATE, 'utf8'); } catch (_) {}
    }
    const merged = deepMerge(DEFAULTS, user);
    // resolve tilde paths
    for (const s of Object.values(merged.tokens.sources)) {
      if (s.dbPath) s.dbPath = expandTilde(s.dbPath);
      if (s.sessionsDir) s.sessionsDir = expandTilde(s.sessionsDir);
      if (s.storageDir) s.storageDir = expandTilde(s.storageDir);
      if (s.usagePath) s.usagePath = expandTilde(s.usagePath);
    }
    this.config = merged;
    this._watch();
    return merged;
  }

  _watch() {
    if (this._watcher) return;
    try {
      this._watcher = fs.watch(APP_DIR, (evt, name) => {
        if (name !== 'config.json') return;
        clearTimeout(this._debounce);
        this._debounce = setTimeout(() => {
          try {
            const user = JSON.parse(stripJsonComments(fs.readFileSync(CONFIG_PATH, 'utf8')));
            const merged = deepMerge(DEFAULTS, user);
            for (const s of Object.values(merged.tokens.sources)) {
              if (s.dbPath) s.dbPath = expandTilde(s.dbPath);
              if (s.sessionsDir) s.sessionsDir = expandTilde(s.sessionsDir);
              if (s.storageDir) s.storageDir = expandTilde(s.storageDir);
              if (s.usagePath) s.usagePath = expandTilde(s.usagePath);
            }
            this.config = merged;
            this.emit('changed', this.config);
          } catch (e) {
            console.error('[wizbar] hot reload failed:', e.message);
          }
        }, 250);
      });
    } catch (e) {
      console.error('[wizbar] config watch failed:', e.message);
    }
  }
}

module.exports = { ConfigManager, CONFIG_PATH, APP_DIR, DEFAULTS, TEMPLATE };
