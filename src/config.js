'use strict';
// WizBar config: defaults merged with %USERPROFILE%\.wizbar\config.json, hot-reloaded on change.
const fs = require('fs');
const path = require('path');
const os = require('os');
const { EventEmitter } = require('events');

const APP_DIR = path.join(os.homedir(), '.wizbar');
const CONFIG_PATH = path.join(APP_DIR, 'config.json');

// Launch with a specific config file: `chocobar --config <path>` (or
// WIZBAR_CONFIG=...). The shipped product stays neutral this way — every
// personal wiring lives in a file you point at, and a plain launch on a fresh
// machine shows the empty defaults. Resolution happens before ConfigManager
// is constructed (main.js passes the result in).
function resolveConfigPath() {
  const args = process.argv.slice(1);
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if (a === '--config' || a === '--config-path') {
      const v = args[i + 1];
      if (v) return expandTilde(v);
    }
    const m = a.match(/^--config(?:-path)?=(.+)$/);
    if (m) return expandTilde(m[1]);
  }
  if (process.env.WIZBAR_CONFIG) return expandTilde(process.env.WIZBAR_CONFIG);
  return CONFIG_PATH;
}

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
    divider: '#D9CCB2',
    // Dashboard surfaces (token dashboard + subscription board) follow the
    // colors above; the five daily-heatmap shades are their own ramp:
    heatmap: ['#F1ECD8', '#F6D8E0', '#EFB7C7', '#E28FB0', '#C95E8F'],
    // Per-surface appearance for the dashboard windows. followBar:true
    // reuses the bar's tint/backdrop; an explicit backgroundTint wins; empty
    // keeps the built-in pink. The dashboards are opaque windows by
    // design (translucent cards read murky over the desktop), so there the
    // tint overrides the color only; the menu is transparent and also honors
    // backgroundAlpha (0 = clear, 255 = solid).
    surfaces: {
      dashboard: { followBar: false, backgroundTint: '', backgroundAlpha: 255 },
      subs: { followBar: false, backgroundTint: '', backgroundAlpha: 255 }
    }
  },
  modules: {
    gpu:      { enabled: true, mode: 'sum', intervalMs: 1200 },
    cpu:      { enabled: true, intervalMs: 800, warnAt: 85 },
    cputemp:  { enabled: true, intervalMs: 2000, warnAt: 85 },  // HWiNFO shm (Windows), sysfs sensors elsewhere
    ram:      { enabled: true, intervalMs: 800, warnAt: 90 },
    volume:   { enabled: true, intervalMs: 500, role: 'multimedia' },
    battery:  { enabled: true, intervalMs: 1500 },
    bluetooth:{ enabled: true, intervalMs: 30000, filter: '', maxDevices: 2, hideWhenEmpty: false },
    // Desktop-pet toggle chip. WINDOWS-ONLY for the on/off state (the pet
    // is a Windows exe tracked through the process list): off in the public
    // build — set enabled + exePath in your own config to use it. Optional
    // "label" replaces the chip's on/off text with "<label> on/off".
    pet:      { enabled: false, exePath: '', label: '' },
    // Leftmost shortcut chip: a button that runs any command you put here
    // (launch an app, open a URL with the default handler, run a script).
    // The label is the chip text; empty shows just the bolt icon. Shipped
    // visible: with no command set it does nothing until you wire one up —
    // set enabled:false to hide the chip.
    shortcut: { enabled: true, label: '', command: '' },
    // User-defined chips: each entry renders a toggle on the bar and runs
    // "command" on click. Design = icon (nerd-font glyph or emoji), label,
    // color; function = command (any shell command or URL). With toggle:true
    // the chip keeps an on/off state and runs "command on" / "command off"
    // (build a script that takes the argument; the state itself is per-run,
    // like the pet's). See the template for a worked example.
    custom: [],
    clock:    { enabled: true, format: '{MMM} {dd} ({Wkk}) {HH}:{mm}' }
  },
  terminal: {
    // Win32 window class to follow. '' (default) = auto-detect the terminal:
    // Windows Terminal, classic conhost, ConEmu, mintty by window class, then
    // WezTerm / Alacritty / Hyper by owning process (see src/tracker.js for
    // the probe order). A string pins one class.
    className: '',
    reattachToExisting: false   // after followed window closes, wait for a NEW window
  },
  tokens: {
    enabled: true,         // master switch: off = no scans, no dashboard data, no chip
    showOnBar: true,       // chip on the bar; with no sources it shows "–"
    rescanMinutes: 1,
    heatmapWeeks: 26,
    // Display names for the harnesses on the dashboard, keyed by source id.
    // Empty = the built-in name. Example: { "opencode": "opencode(wsl)" }.
    labels: { zcode: '', zai: '', pi: '', opencode: '', mimo: '' },
    // Which sections of the token dashboard are visible. Every section can be
    // switched off; the window always keeps its fixed size either way.
    dashboard: {
      stats: true,      // Today / last 7 days / last 30 days / all time cards
      heatmap: true,    // per-day usage heatmap (with the weekday ruler)
      dayDetail: true,  // per-harness breakdown shown when you click a day
      apps: true,       // "By app" table
      models: true,     // "By model" table
      plans: true       // plan-usage card (needs sources.subscription enabled)
    },
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
  // Subscription board: live plan-quota windows (rate limits / credits) for
  // whatever subscriptions you wire up. Each provider entry names an adapter
  // type and where its credential lives; the two examples ship disabled. The
  // gauge chip ships visible: it reads "—" until you enable a provider.
  subs: {
    enabled: true,
    intervalMinutes: 2,
    // Per-provider request deadline in ms (clamped 3s..60s, same as the
    // global fetchTimeoutMs). A provider that answers slower is skipped for
    // that cycle with an error note; the board keeps showing its last good
    // windows marked stale.
    fetchTimeoutMs: 20000,
    // Board window size (fixed; the board is not resizable).
    width: 820,
    height: 480,
    providers: [
      // ChatGPT plan via a Codex CLI login (rate-limit windows, no numbers):
      { type: 'chatgpt', enabled: false, label: 'ChatGPT', authPath: '~/.codex/auth.json' },
      // Z.ai coding plan via the zcode credential (5h + weekly quota windows):
      { type: 'zai', enabled: false, label: 'Z.ai', configPath: '~/.zcode/v2/config.json',
        provider: 'builtin:zai-coding-plan' },
      // Google Antigravity plan (Cloud Code quota; token auto-refreshes from
      // the pi OAuth store, or the IDE token when vscdbPath is set):
      { type: 'antigravity', enabled: false, label: 'Antigravity',
        authPath: '~/.pi/agent/auth.json' }
    ]
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
    "divider": "#D9CCB2",
    // the five daily-heatmap shades on the token dashboard, light to dark
    "heatmap": ["#F1ECD8", "#F6D8E0", "#EFB7C7", "#E28FB0", "#C95E8F"],
    // Per-surface appearance for the dashboard windows. "followBar": true
    // reuses the bar's tint + backdrop; otherwise "backgroundTint" wins,
    // empty = the built-in pink. The dashboards are opaque windows by design
    // (translucent cards read murky over the desktop), so the tint overrides
    // the color only.
    "surfaces": {
      "dashboard": { "followBar": false, "backgroundTint": "", "backgroundAlpha": 255 },
      "subs":      { "followBar": false, "backgroundTint": "", "backgroundAlpha": 255 }
    }
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
    // Desktop-pet toggle chip (WINDOWS-ONLY for the on/off state, off by
    // default). Set enabled + exePath here to show the bow chip: click launches
    // the exe, click again stops it ("on"/"off"). Optional "label" prefixes the
    // chip text with the pet's name.
    "pet":       { "enabled": false, "exePath": "", "label": "" },
    // Shortcut chip, leftmost in the bar: a bolt button that runs "command"
    // (any program, script or URL your shell can launch). "label" is the chip
    // text; empty shows just the bolt icon. Visible by default; set false to
    // hide it.
    "shortcut":  { "enabled": true, "label": "", "command": "" },
    // User-defined chips: add as many as you like, each is one object in the
    // "custom" list. A click runs "command" (any program, script or URL your
    // shell can launch).
    //   icon    nerd-font glyph or emoji shown before the label (empty = none)
    //   label   chip text (empty = icon-only chip)
    //   color   chip text/icon color (empty = theme default)
    //   title   hover tooltip (empty = label)
    //   toggle  true = the chip holds an on/off state and runs
    //           "command on" / "command off" so your script can react
    // Example (ships disabled - flip enabled:true to try it):
    "custom": [
      { "enabled": false, "icon": "", "label": "Focus", "color": "",
        "title": "Toggle the focus script", "toggle": true,
        "command": "C:\\tools\\focus.bat" }
    ],
    // {MMM} month, {dd} day, {Wkk} weekday (Mon..Sun), {HH} {mm} {ss} time (24h)
    "clock":     { "enabled": true, "format": "{MMM} {dd} ({Wkk}) {HH}:{mm}" }
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
    "enabled": true,
    "showOnBar": true,
    // minutes between usage scans (drives the dashboard's live refresh)
    "rescanMinutes": 1,
    "heatmapWeeks": 26,
    // display names for the harnesses on the dashboard, keyed by source id;
    // empty = built-in name (e.g. { "opencode": "opencode(wsl)" })
    "labels": { "zcode": "", "zai": "", "pi": "", "opencode": "", "mimo": "" },
    // which dashboard sections are visible (true/false each)
    "dashboard": {
      "stats": true,      // Today / 7d / 30d / all-time cards
      "heatmap": true,    // per-day usage heatmap
      "dayDetail": true,  // breakdown shown when you click a day
      "apps": true,       // "By app" table
      "models": true,     // "By model" table
      "plans": true       // plan-usage card (needs sources.subscription on)
    },
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
  // Subscription board: live plan-quota windows for whatever subscriptions
  // you wire up. Each entry names an adapter "type" (chatgpt | zai |
  // antigravity), a display label and where its credential lives. The board
  // ships on with all examples disabled: the gauge chip reads "—" until you
  // enable one. Add or remove entries freely - any number of plans renders.
  "subs": {
    "enabled": true,
    "intervalMinutes": 2,
    // per-provider request deadline in ms (3s..60s); on timeout the board
    // keeps the provider's last good windows and marks them stale
    "fetchTimeoutMs": 20000,
    // board window size (fixed, not resizable)
    "width": 820,
    "height": 480,
    "providers": [
      // ChatGPT plan via a Codex CLI login (rate-limit windows)
      { "type": "chatgpt", "enabled": false, "label": "ChatGPT", "authPath": "~/.codex/auth.json" },
      // Z.ai coding plan via the zcode credential (5h + weekly windows)
      { "type": "zai", "enabled": false, "label": "Z.ai", "configPath": "~/.zcode/v2/config.json",
        "provider": "builtin:zai-coding-plan" },
      // Google Antigravity plan (Cloud Code quota; the pi OAuth token
      // auto-refreshes, or set "vscdbPath" for the IDE's stored token)
      { "type": "antigravity", "enabled": false, "label": "Antigravity",
        "authPath": "~/.pi/agent/auth.json" }
    ]
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

function expandConfigPaths(cfg) {
  for (const s of Object.values(cfg.tokens.sources)) {
    if (s.dbPath) s.dbPath = expandTilde(s.dbPath);
    if (s.sessionsDir) s.sessionsDir = expandTilde(s.sessionsDir);
    if (s.storageDir) s.storageDir = expandTilde(s.storageDir);
    if (s.usagePath) s.usagePath = expandTilde(s.usagePath);
  }
  for (const p of (cfg.subs && cfg.subs.providers) || []) {
    if (p.authPath) p.authPath = expandTilde(p.authPath);
    if (p.configPath) p.configPath = expandTilde(p.configPath);
  }
  return cfg;
}

class ConfigManager extends EventEmitter {
  constructor(configPath) {
    super();
    this.config = null;
    this.path = configPath || resolveConfigPath();
    this._watcher = null;
    this._debounce = null;
  }

  load() {
    const dir = path.dirname(this.path);
    if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
    let user = {};
    if (fs.existsSync(this.path)) {
      try {
        user = JSON.parse(stripJsonComments(fs.readFileSync(this.path, 'utf8')));
      } catch (e) {
        console.error('[wizbar] config parse error, using defaults:', e.message);
      }
    } else {
      // First use of this config path: write the annotated template so there
      // is always a file to open with "Edit config" — on the default path and
      // on an explicit --config path alike.
      try { fs.writeFileSync(this.path, TEMPLATE, 'utf8'); } catch (_) {}
    }
    const merged = expandConfigPaths(deepMerge(DEFAULTS, user));
    this.config = merged;
    this._watch();
    return merged;
  }

  _watch() {
    if (this._watcher) return;
    try {
      this._watcher = fs.watch(path.dirname(this.path), (evt, name) => {
        if (name !== path.basename(this.path)) return;
        clearTimeout(this._debounce);
        this._debounce = setTimeout(() => {
          try {
            const user = JSON.parse(stripJsonComments(fs.readFileSync(this.path, 'utf8')));
            this.config = expandConfigPaths(deepMerge(DEFAULTS, user));
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

module.exports = { ConfigManager, CONFIG_PATH, APP_DIR, DEFAULTS, TEMPLATE, resolveConfigPath };
