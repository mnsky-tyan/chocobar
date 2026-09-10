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
    radius: 8,
    roundCorners: true,    // all four corners (adds the subtle floating shadow)
    insetX: 2,             // DIP shaved per side so the bar doesn't overhang the terminal frame
    fontSize: 11,
    fontFamily: "'MesloLGLDZ Nerd Font', 'Cascadia Mono', Consolas, monospace",
    align: 'right',        // right | left | center
    position: 'above',     // above | below
    backdrop: 'acrylic',   // acrylic | solid | none
    backgroundTint: '#FBF2E2',   // pale yellow/pink blend to match terminal acrylic
    backgroundAlpha: 110,        // 0-255 — ACTUAL fill opacity over the blur (0 = clear)
    segmentSpacing: 14
  },
  theme: {
    fg: '#080808',
    fgDim: '#5a5245',
    pink: '#E8C7D0',
    pinkDeep: '#C77B96',
    pinkBg: '#FDEFF2',
    yellow: '#B8A96A',
    yellowBg: '#F5F0D8',
    warn: '#A00000',
    good: '#006400',
    divider: '#D9CCB2'
  },
  modules: {
    gpu:      { enabled: true, mode: 'sum', intervalMs: 1200 },
    cpu:      { enabled: true, intervalMs: 800, warnAt: 85 },
    ram:      { enabled: true, intervalMs: 800, warnAt: 90 },
    volume:   { enabled: true, intervalMs: 500, role: 'multimedia' },
    battery:  { enabled: true, intervalMs: 1500 },
    bluetooth:{ enabled: true, intervalMs: 30000, filter: '', maxDevices: 2, hideWhenEmpty: false },
    clock:    { enabled: true, format: '{MMM} {dd}  {HH}:{mm}' }
  },
  terminal: {
    className: 'CASCADIA_HOSTING_WINDOW_CLASS',
    reattachToExisting: false   // after followed window closes, wait for a NEW window
  },
  tokens: {
    enabled: true,
    showOnBar: true,
    rescanMinutes: 5,
    heatmapWeeks: 26,
    sources: {
      zcode:    { enabled: true, dbPath: '~/.zcode/cli/db/db.sqlite' },
      zai:      { enabled: true, sessionsDir: '~/.zai/agent/sessions' },
      opencode: { enabled: true, storageDir: '~/.local/share/opencode/storage/message' }
    }
  },
  general: {
    showTray: true,
    autostart: false,
    debug: false
  }
};

// The config file users edit — same as DEFAULTS but with inline explanations.
// Save the file and the bar hot-reloads within a second; no restart needed.
const TEMPLATE = `// WizBar config — edit any value and save; changes apply live.
// Colors are CSS hex (#RRGGBB). Sizes are DIP (CSS pixels).
{
  "bar": {
    // height of the bar strip
    "height": 24,
    // air between the bar and the terminal's top edge (the floating gap)
    "gap": 16,
    // trim per side so the bar doesn't overhang the terminal frame (0 to disable)
    "insetX": 2,
    // rounded corners (Win11); also adds a subtle floating shadow
    "roundCorners": true,
    // text size + font (must be an installed font)
    "fontSize": 11,
    "fontFamily": "'MesloLGLDZ Nerd Font', 'Cascadia Mono', Consolas, monospace",
    // where the modules sit: right | left | center
    "align": "right",
    // bar above or below the terminal window
    "position": "above",
    // "acrylic" = blurred see-through, "solid" = opaque, "none" = clear
    "backdrop": "acrylic",
    // the tint color of the box
    "backgroundTint": "#FBF2E2",
    // box fill opacity over the blur: 0 = fully clear, 255 = solid color.
    // lower = more transparent. ~110 is airy, ~180 is creamy.
    "backgroundAlpha": 110,
    // spacing between the modules (tokens, cpu, ...)
    "segmentSpacing": 14
  },
  "theme": {
    "fg": "#080808",
    "fgDim": "#5a5245",
    "pink": "#E8C7D0",
    "pinkDeep": "#C77B96",
    "pinkBg": "#FDEFF2",
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
    "ram":       { "enabled": true, "intervalMs": 800, "warnAt": 90 },
    "volume":    { "enabled": true, "intervalMs": 500, "role": "multimedia" },
    "battery":   { "enabled": true, "intervalMs": 1500 },
    // earbud/BT battery; shows "—" unless a device reports via Windows' standard
    // battery property (many earbuds only report to their vendor app)
    "bluetooth": { "enabled": true, "intervalMs": 30000, "filter": "", "maxDevices": 2 },
    // {MMM} month, {dd} day, {HH} {mm} {ss} time (24h)
    "clock":     { "enabled": true, "format": "{MMM} {dd}  {HH}:{mm}" }
  },
  "terminal": {
    // Win32 class of the window to follow
    "className": "CASCADIA_HOSTING_WINDOW_CLASS",
    // false: after the followed window closes, only a NEW terminal re-triggers the bar.
    // true: the bar grabs whatever terminal already exists instead.
    "reattachToExisting": false
  },
  "tokens": {
    "enabled": true,
    "showOnBar": true,
    "rescanMinutes": 5,
    "heatmapWeeks": 26,
    "sources": {
      "zcode":    { "enabled": true, "dbPath": "~/.zcode/cli/db/db.sqlite" },
      "zai":      { "enabled": true, "sessionsDir": "~/.zai/agent/sessions" },
      "opencode": { "enabled": true, "storageDir": "~/.local/share/opencode/storage/message" }
    }
  },
  "general": {
    "showTray": true,
    // start WizBar at login (writes an HKCU Run entry)
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
