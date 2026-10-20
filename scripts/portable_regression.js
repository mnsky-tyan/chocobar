'use strict';
// Regression guards for the SHIPPED product: the native Win32 bar. This suite
// used to cover the retired Electron app's portability layer and pure logic
// (sysfs readers, metrics dirty tracking, JS defaults, the token tracker);
// that tree is gone and its guards went with it. What remains must hold for
// every release: the first-run config template the native bar writes
// (g_template in native/src/p_ui.c, read as JSONC by the C parser) is valid
// JSONC and ships NEUTRAL defaults - nothing read, nothing personal.
//
//   node scripts/portable_regression.js

const fs = require('fs');
const path = require('path');
// Each source file is read ONCE up front; the thematic blocks below work from
// this snapshot instead of re-loading the same file inside their own scope.
const sources = {
  p_ui: fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'p_ui.c'), 'utf8'),
  p_tokens: fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'p_tokens.c'), 'utf8'),
  chocobar: fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'chocobar.c'), 'utf8'),
};

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}${detail ? '  [' + detail + ']' : ''}`);
  if (!ok) failures++;
}

// --- native first-run template neutrality -------------------------------------
// The native bar writes its OWN starting file (g_template in native/src/p_ui.c).
// It must ship neutral: every usage source and board provider off, so an
// untouched install reads nothing and makes no request. Decode the C string,
// strip the JSONC comments and assert meaning from the parsed object.
{
  const c = sources.p_ui;
  const m = c.match(/static const char \*g_template =([\s\S]*?);\n/);
  const decode = (body) => (body.match(/"((?:[^"\\]|\\.)*)"/g) || [])
    .map((s) => s.slice(1, -1))
    .map((s) => s.replace(/\\(.)/g, (_m, ch) => {
      // fail loudly: a mishandled escape would let the neutrality checks pass
      // against mangled text instead of the shipped template
      const map = { n: '\n', r: '\r', t: '\t', '"': '"', '\\': '\\' }[ch];
      if (map === undefined) throw new Error('unsupported escape \\' + ch + ' in g_template - teach decode() about it');
      return map;
    }))
    .join('');
  const stripComments = (s) => {
    let out = '', inStr = false;
    for (let i = 0; i < s.length; i++) {
      const ch = s[i];
      if (inStr) { out += ch; if (ch === '"') inStr = false; continue; }
      if (ch === '"') { inStr = true; out += ch; continue; }
      if (ch === '/' && s[i + 1] === '/') {
        while (i < s.length && s[i] !== '\n') i++;
        out += '\n';
        continue;
      }
      out += ch;
    }
    return out;
  };
  let tpl = null, err = '';
  if (!m) {
    check('template: g_template found in p_ui.c', false);
  } else {
    try { tpl = JSON.parse(stripComments(decode(m[1]))); } catch (e) { err = String((e && e.message) || e); }
    check('template: parses as JSONC (braces balance, every string closes)', tpl !== null, err);
  }
  if (tpl) {
    const tok = tpl.tokens || {};
    const mod = tpl.modules || {};
    const srcs = Array.isArray(tok.sources) ? tok.sources : [];
    check('template: every documented root key present',
      ['bar', 'theme', 'dashboard', 'tokens', 'modules', 'subs', 'terminal', 'general']
        .every((k) => k in tpl));
    check('template: tokens master off and every source off',
      tok.enabled === false && srcs.length > 0 && srcs.every((s) => s.enabled === false),
      `enabled=${tok.enabled} sources=${srcs.length} on=${srcs.filter((s) => s.enabled).length}`);
    // a source path is a directory of JSONL files the in-process scan walks; a
    // SQLite database is NOT one (that store arrives via tokens.cachePath), so
    // such an entry ships a switch that silently does nothing
    check('template: no source points at a SQLite database',
      srcs.every((s) => !/\.(sqlite|sqlite3|db)$/i.test(String(s.path || ''))));
    check('template: pet chip and subs board ship off',
      !!(mod.pet && mod.pet.enabled === false) && !!(tpl.subs && tpl.subs.enabled === false));
    // the estimator of blind turns: an estimate is not a measurement, so the
    // template ships it ON and says so, and its constants stay overridable in
    // the config file rather than being baked into the source
    check('template: estimateMissingUsage on by default',
      tok.estimateMissingUsage === true);
    check('template: estimate block absent by default (built-ins apply)',
      !('estimate' in tok));
    check('template: no personal identifiers in the template',
      !/tyanw|mnsky|firstmate|captain|crewmate|remielle/i.test(decode(m[1])));
    // No provider may ship a credential path that names a specific third-party
    // harness unless that harness IS the provider (chatgpt/zai ship their own
    // tool's default). The antigravity entry once shipped another agent tool's
    // auth path, which no ordinary user has, and a refreshed token is written
    // back to whatever path is configured - so a vendor name there is a real
    // hazard, not just a bad example.
    const provs = (tpl.subs && Array.isArray(tpl.subs.providers)) ? tpl.subs.providers : [];
    check('template: antigravity ships no default credential path',
      provs.filter((p) => p.type === 'antigravity').every((p) => !p.authPath),
      JSON.stringify(provs.filter((p) => p.type === 'antigravity').map((p) => p.authPath)));
  }
}

// --- C-side estimator guards --------------------------------------------------
// The estimator (native/src/p_tokens.c) decides WHICH lines are counted and how,
// and the config parser (native/src/chocobar.c) decides the constants. Both are
// easy to break in a way that only shows up as a slightly wrong number months
// later, so pin the properties that make the estimate honest.
{
  const pt = sources.p_tokens;
  const cb = sources.chocobar;

  // a bridge turn must be judged by its route, not by a hardcoded provider list:
  // mimo rows have been toggled off and on, and workbuddy-2 appeared later, so any
  // such list is wrong the day a route changes. The code consults the set of
  // routes that DID report in the same file, no route name is compiled in, and
  // the match is an exact comma-delimited element test (a substring test matched
  // "mimo" inside "mimo-desktop" and read a blind turn on the shorter route as
  // an aborted reporting one). tokRealHas delegates that judgment to the shared
  // tokProvIn matcher, which compares whole elements case-folded.
  check('estimator: blindness is decided from the file\'s own reporting routes',
    /tokRealHas\(real, pv\)/.test(pt) && /blind = 0/.test(pt)
    && /return tokProvIn\(real, pv, \(int\)strlen\(pv\)\)/.test(pt)
    && /static int tokProvIn\(const char \*list, const char \*pv, int pl\)/.test(cb)
    && /\(int\)\(segEnd - p\) == pl && tokNIeq\(p, pv, pl\)/.test(cb)
    && !/strstr\(real, pv\)/.test(pt),
    'an exact comma-delimited element match, never a substring test');
  check('estimator: no provider name is compiled in as a quoted literal',
    !/"(workbuddy|mimo[-a-z]*|antigravity|codebuddy)"/i.test(pt),
    'a bridge list would rot the moment a route changes');

  // the running total must persist, or an appended read would restart the
  // transcript from zero and estimate the tail as if the session were tiny
  check('estimator: chars/real persist in the cursor file',
    /"chars"/.test(pt) && /"real"/.test(pt) && /cur->chars = chars/.test(pt));

  // saturation is what stops a long transcript estimating at tens of millions of
  // tokens; without it a single turn is larger than any context window
  check('estimator: prompt is capped', /if \(prompt > saturate\) prompt = saturate;/.test(pt));

  // it must never replace a turn that DID report: a positive usage sum goes
  // through the normal path unmarked, and only a sum of zero reaches the
  // estimate, whose rows are the only ones flagged as estimated
  check('estimator: only all-zero usage is estimated',
    /if \(sum > 0\) \{/.test(pt) && /else if \(estimating\) \{/.test(pt)
    && /r\.model, r\.modelLen, 0\);/.test(pt) && /r\.model, r\.modelLen, 1\);/.test(pt),
    'a real row is never flagged, an estimated row always is');

  // on by default, with the constants in config and a sane floor
  check('config: estimateMissingUsage defaults to on',
    /tokensEnabled = jboolDefault\(js, t, jobjGet\(js, t, toks, "enabled"\), 1\);[\s\S]{0,1500}tokEstimate = jboolDefault\(js, t, jobjGet\(js, t, toks, "estimateMissingUsage"\), 1\);/.test(cb));
  check('config: the three constants are configurable',
    /"inputFactor"/.test(cb) && /"saturateTokens"/.test(cb) && /"outputFactor"/.test(cb));
  check('config: constants fall back on a zero/negative value',
    /if \(c->tokEstIn\s+<= 0\)/.test(cb) && /if \(c->tokEstSat\s+<= 0\)/.test(cb)
    && /if \(c->tokEstOut\s+<= 0\)/.test(cb));
}

// --- version sync -------------------------------------------------------------
// version.h calls itself the single source of truth for the shipped version,
// but package.json and package-lock.json repeat the number by hand, and git
// history shows the copies drifting apart on hand bumps. The binary only ever
// reads version.h, so pin the three-way agreement here: a bump that misses a
// copy fails the suite instead of shipping disagreeing metadata.
{
  const vh = fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'version.h'), 'utf8');
  const m = vh.match(/#define CB_VER_STR\s+"([^"]+)"/);
  const pkg = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'package.json'), 'utf8'));
  const lock = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'package-lock.json'), 'utf8'));
  const lockRoot = lock.packages && lock.packages[''] ? lock.packages[''].version : undefined;
  check('version: version.h declares CB_VER_STR', !!m);
  // The numeric triple feeds version.rc's FILEVERSION/PRODUCTVERSION, which no
  // other check reads: a bump that edits only CB_VER_STR would ship an exe whose
  // string resource says one version and whose numeric file version says another.
  // Match an anchored, ACTIVE define and require exactly one: the preprocessor
  // uses the last effective definition and ignores commented-out text, so a
  // first-textual-match regex would accept a stray duplicate or a commented one.
  const nums = ['MAJOR', 'MINOR', 'PATCH'].map((k) => {
    const active = vh.split('\n').filter((line) => new RegExp(`^#define CB_VER_${k}\\s+\\d+`).test(line));
    const n = active.length === 1 ? active[0].match(new RegExp(`^#define CB_VER_${k}\\s+(\\d+)`)) : null;
    return n ? n[1] : null;
  });
  check('version: the numeric triple matches CB_VER_STR',
    !!m && nums.every((n) => n !== null) && nums.join('.') === m[1],
    `numeric=${nums.join('.')} str=${m ? m[1] : '?'}`);
  if (m) {
    check('version: package.json, package-lock.json, and version.h agree',
      pkg.version === m[1] && lock.version === m[1] && lockRoot === m[1],
      `version.h=${m[1]} package.json=${pkg.version} lock=${lock.version}/${lockRoot}`);
  }
}

// --- autostart follows the exe when it moves ----------------------------------
// The Run value is written first-run-only from whatever path the exe was
// launched out of. The shipped app is a single portable exe a user may move
// after running it once, so a stored path that no longer matches the running
// one must be repaired - otherwise every boot starts a stale copy, or nothing
// at all when that copy is gone. The heal must (a) rewrite on a mismatch, and
// (b) leave an absent value alone, because absence is the user's "off".
{
  const ui = sources.p_ui;
  const heal = ui.match(/static void autoStartHeal\(void\) \{([\s\S]*?)\n\}/);
  check('autostart: a moved exe is rewritten on mismatch',
    !!heal && /lstrcmpiW\(cur, want\)/.test(heal[1]) && !/lstrcmpW\(cur, want\)/.test(heal[1]),
    'case-insensitive compare: Windows paths differ in case');
  check('autostart: an absent Run value is left alone',
    !!heal && /RegGetValueW\([^]*?!= ERROR_SUCCESS\) return;/.test(heal[1]),
    'no value = the user turned it off; healing it back on would be wrong');
  check('autostart: the heal runs on every load after the first-run write',
    /if \(freshInstall && g_cfg\.autoStart\) autoStartSet\(1\);\s*\n\s*else autoStartHeal\(\);/.test(ui),
    'first run writes, every later run repairs');
}

// --- the live token scan re-stages history whenever the seed moves -------------
// The dashboard's totals are two halves: the Electron cache (the seed, holding
// history) and the live JSONL scan, which stages only records NEWER than the
// seed's boundary. On a seed re-read the drain wipes the aggregates and rebuilds
// them from the staged records alone, so that scan MUST read every file from
// byte 0 - resuming from a per-file cursor would omit the records above the
// boundary that sit earlier in each file, and the history would vanish from the
// board. Measured 2026-10-10: without this, a restart collapsed ~9.0B to 3.5M.
// The inverse matters too: forcing it on EVERY scan re-adds the whole history
// each pass (measured 9.0B -> 36.0B -> 45.0B). Replay is required at startup,
// when the boundary moves, or when a seed reread actually resets aggregates.
// Unchanged warm scans must stay incremental. Executable restart/reset
// coverage lives in token_regression.py; these older text guards are retained.
{
  const tk = sources.p_tokens;
  const seed = tk.match(/void tokLiveSeed\(long long cacheMaxTs[\s\S]*?\n\}/);
  check('tokens: a moved seed boundary forces a full re-read',
    !!seed && /if \(g_cacheMaxTs != cacheMaxTs\) g_tokForceFullRead = 1;/.test(seed[0]),
    'a seed re-read rebuilds the live half from scratch, so cursors must be bypassed');
  check('tokens: seed handling does NOT force an unconditional full re-read',
    !!seed && !/^\s*g_tokForceFullRead = 1;\s*$/m.test(seed[0]),
    'unconditional would re-add all history every 60s and inflate the totals');
  check('tokens: the scan honours the forced full read',
    /if \(c && !g_tokFullReadNow && c->size == fsz/.test(tk)
      && /long long from = \(c && !g_tokFullReadNow\) \? c->size : 0;/.test(tk),
    'both the unchanged-file skip and the resume offset must bypass the cursor');
  check('tokens: the forced flag is consumed by the scan, not left set',
    /int fullRead = g_tokForceFullRead;\s*\n\s*g_tokForceFullRead = 0;/.test(tk),
    'left set, every later rescan would re-read from byte 0');
}

// --- Linux bar: the invariants a reviewer cannot see in a screenshot -------------
// These are the three bugs found while bringing the bar up on this machine, each
// of which produced a window that LOOKED right and did not work. They are cheap
// text guards because each one is a single missing line whose absence is silent.
{
  const lx = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'main.c'), 'utf8');
  check('linux: the window selects an ARGB visual',
    /XMatchVisualInfo\(g_dpy, g_screen, 32, TrueColor, &vinfo\)/.test(lx)
      && /g_depth = 32;/.test(lx),
    'on the default 24-bit visual the alpha channel is dropped and the bar renders as a dark grey strip');
  check('linux: the window passes CWEventMask',
    /CWBackPixel \| CWBorderPixel \|\s*\n?\s*CWColormap \| CWEventMask/.test(lx),
    'without it the server drops every button event: visible, correct and completely inert');
  check('linux: each frame clears the surface and the text path',
    /CAIRO_OPERATOR_SOURCE/.test(lx) && /cairo_new_path\(g_cr\);\s*\n\s*cairo_set_operator\(g_cr, CAIRO_OPERATOR_OVER\);/.test(lx),
    'a bar repainted in place accumulates glyphs from earlier frames');
  check('linux: the shape comes from the rectangles it builds',
    /XFixesCreateRegion\(g_dpy, rects, nr\)/.test(lx)
      && /XFixesSetWindowShapeRegion\(g_dpy, g_win, ShapeBounding, 0, 0, g_region\)/.test(lx),
    'building a rounded shape and then shaping to an empty region makes the bar unclickable');
}

// --- shared icon geometry ----------------------------------------------------------
// The icon paths moved out of p_icons.c into p_svg.c so the Windows GDI+ renderer
// and the Linux cairo renderer stroke the SAME geometry. A renderer that grew its
// own copy would drift, and the icons are the product's look.
{
  const svg = fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'p_svg.c'), 'utf8');
  const icons = sources.p_ui.replace(/x/g, 'x'); // keep the snapshot usage explicit
  check('svg: the path data lives in p_svg.c, not the Windows renderer',
    /static const SvgPart SVG_P\[\]\[3\] = \{/.test(svg)
      && !/static const SvgPart SVG_P\[\]\[3\] = \{/.test(icons));
  check('svg: both renderers include the shared geometry header',
    /#include "svg_geometry.h"/.test(fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'p_icons.c'), 'utf8'))
      && /#include "svg_geometry.h"/.test(fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'icons_cairo.c'), 'utf8')),
    'one shape definition, one flattener, two strokes');
  check('svg: the flattener is what both builds call',
    /static void svgFlattenAll\(void\)/.test(svg)
      && /svgFlattenAll\(\);/.test(fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'p_icons.c'), 'utf8')),
    'the Windows startup path must reach the shared flattener, not a local copy');
}

// --- bar.align and the geometry bridge ----------------------------------------------
// "As long as the terminal" needs two halves that must agree on their contract:
// the KWin helper publishes (class, width, x, y, screenWidth) in LOGICAL pixels,
// and the bar scales them. A mismatch here is invisible (a bar of the wrong width)
// rather than an error, so it is guarded.
{
  const lx = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'main.c'), 'utf8');
  const js = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'geomwatch.js'), 'utf8');
  const cf = fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'chocobar.c'), 'utf8');
  check('align: bar.align is parsed and clamped to 0..2',
    /c->barAlign\s*=\s*jintTok\(js, t, jobjGet\(js, t, bar, "align"\)/.test(cf)
      && /if \(c->barAlign > 2\) c->barAlign = 2;/.test(cf),
    '0 = centre, 1 = match the active window, 2 = full screen');
  check('align: the helper sends the five fields the bar decodes',
    /callDBus\("org\.chocobar\.Geom", "\/ChocobarGeom", "org\.chocobar\.Geom", "Report",/.test(js)
      && /DBUS_TYPE_STRING, &cls,[\s\S]{0,400}DBUS_TYPE_INT32, &width,[\s\S]{0,400}DBUS_TYPE_INT32, &x,[\s\S]{0,400}DBUS_TYPE_INT32, &y,[\s\S]{0,400}DBUS_TYPE_INT32, &screenW/.test(
           fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'geomdbus.c'), 'utf8')),
    'a field added on one side and not the other silently misaligns the width');
  check('align: the bar scales logical geometry into device pixels',
    /\*outW = \(int\)\(g_geom\.width \* g_scale \+ 0\.5\)/.test(lx)
      && /\*outX = \(int\)\(g_geom\.x \* g_scale \+ 0\.5\)/.test(lx),
    'KWin reports logical px; the X window is device px');
  check('align: a stale geometry report is not trusted',
    /time\(NULL\) - g_geomAt < 30/.test(lx),
    'a KWin script that stopped must not pin the bar to a window that is gone');
  check('align: the bar never gets narrower than its chips',
    /if \(wantW < chipsW\) wantW = chipsW;/.test(lx),
    'a terminal narrower than the readouts would clip them');
  check('linux: a file source is scanned, not silently skipped',
    /static void tokScanFileSource/.test(fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'p_tokens.c'), 'utf8'))
      || /tokScanDir\(parent, 0, s->app, &tk, cfg\)/.test(fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'p_tokens.c'), 'utf8')),
    'magpie keeps ONE ledger file; the walk only enumerated directories, so that source contributed nothing');
}

// --- the Linux board, menu and font -------------------------------------------------
// Each guard below is a bug that shipped in the first Linux cut and that a
// screenshot could not catch: a dashboard that silently created no window, a menu
// that closed itself, a font setting that was ignored, and an X resource leak.
{
  const lx = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'main.c'), 'utf8');
  const menu = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'menu.c'), 'utf8');
  const dash = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'dash.c'), 'utf8');
  const icons = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'icons_cairo.c'), 'utf8');
  check('linux: the cairo context is destroyed before its surface',
    /if \(g_cr\) \{ cairo_destroy\(g_cr\); g_cr = NULL; \}\s*\n\s*if \(g_surf\) \{ cairo_surface_destroy\(g_surf\); g_surf = NULL; \}/.test(lx),
    'a context outliving its surface raised thousands of RenderBadPicture errors');
  check('linux: the dashboard uses the default visual, not the bar ARGB one',
    /Visual \*dvis = DefaultVisual\(g_dpy, g_screen\)/.test(lx),
    'an ARGB visual with depth 24 makes XCreateWindow fail silently: no window at all');
  check('linux: the menu does not close on FocusOut',
    /case FocusOut:[\s\S]{0,600}?break;/.test(menu) && !/case FocusOut:\s*\n\s*\/\/[^\n]*\n\s*done = 1;/.test(menu),
    'an immediate FocusOut closed the menu a millisecond after it opened');
  check('linux: the menu coalesces Expose instead of repainting per event',
    /while \(XPending\(dpy\)\)/.test(menu),
    'painting from inside an Expose handler spun an expose storm');
  check('linux: the font goes through fontconfig, not a hand-parsed name',
    /FcNameParse\(/.test(lx) && /FcFontMatch\(/.test(lx),
    'cairo toy select_font_face ignores CSS stacks, aliases and generic families');
  check('linux: the battery is drawn dynamically',
    /void svgDrawBattery\(/.test(icons) && /svgDrawBattery\(g_cr, g_m\.battPct, g_m\.battAc/.test(lx),
    'the static shell shows no charge level, no low-red and no charging bolt');
  check('linux: the dashboard reads the SHARED aggregates',
    /extern long long cbTokToday\(void\)/.test(dash) && /cbTokAll\(\)/.test(dash),
    'a dashboard with its own totals would disagree with the Windows board');
}

// --- the bar's layer and its (absent) drag -------------------------------------------
// KWin composites override-redirect windows in its TOP layer no matter which
// _NET_WM_STATE hints they carry (measured on Plasma 6.6, 2026-10-19: every
// override-redirect probe, with or without BELOW/DESKTOP/DOCK hints, painted over
// a raised terminal). The bar belongs at the LOWER layer - it must sit under
// normal windows - so it is a managed window with _NET_WM_STATE_BELOW, and the
// two properties that make a managed window behave like an unmanaged one
// (no decorations, no placement policy) must both stay set.
{
  const lx = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'main.c'), 'utf8');
  check('linux: the bar is managed, not override-redirect',
    !/swa\.override_redirect\s*=\s*True/.test(lx) && !/CWOverrideRedirect/.test(lx),
    'override-redirect forces the top layer in KWin; a managed BELOW window is the only way under');
  check('linux: the bar asks for the BELOW layer',
    /_NET_WM_STATE_BELOW/.test(lx) && /_MOTIF_WM_HINTS/.test(lx),
    'BELOW puts it under normal windows; MOTIF removes the frame a managed window would get');
  check('linux: the bar never takes keyboard focus',
    /hints\.input = False/.test(lx) && /_NET_WM_USER_TIME/.test(lx),
    'a managed window that accepts focus would pull the keyboard off the terminal on map or click');
  const motion = /static void onMotion\(XMotionEvent \*e\) \{([\s\S]*?)\n\}/.exec(lx);
  check('linux: the bar is not draggable',
    !/g_dragging/.test(lx) && !/\bsavePos\b/.test(lx) && !/linux-bar\.json/.test(lx) &&
    !!motion && !/XMoveWindow|g_x\s*=|g_y\s*=/.test(motion[1]),
    'its position is derived from bar.align and bar.gap; a dragged bar breaks that contract');

  // the shape's middle band must be built from integer steps, not a truncating
  // float: (unsigned short)(h - 2*radius) floors 22.56 to 22, leaving one row
  // uncovered between the band and the bottom loop - a 1 px horizontal hole
  // through which raw wallpaper shows as a bright line across the bar.
  const shp = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'linux_shape.c'), 'utf8');
  check('linux: the shape covers every row of the bar',
    !/\(unsigned short\)fmax\(1\.0, [wh] - 2 \* radius\)/.test(shp) &&
    /\(unsigned short\)fmax\(1, h - 2 \* steps\)/.test(shp),
    'a truncating float leaves a 1 px gap the shape clips away; the line is wallpaper, not paint');

  // layout: the demo has TWO groups - left-pinned chips (tokens) and right-aligned
  // metrics, with an empty run between them. A single right-aligned loop packs every
  // chip against the right edge and reads as one block, which is the visual gap
  // that was reported against docs/img/bar-terminal.png.
  check('linux: the bar lays out both chip groups',
    /CHIP_LEFT/.test(lx) && /CHIP_RIGHT/.test(lx) &&
    /double xl = padL;/.test(lx) && /xr = g_w - padR;/.test(lx) &&
    /align != CHIP_LEFT\) continue;/.test(lx) && /align != CHIP_RIGHT\) continue;/.test(lx),
    'a single right-aligned loop ignores the pinned group and breaks demo parity');
  check('linux: only the left group is clickable',
    /chipClickable/.test(lx) && /c->align == CHIP_LEFT && c->icon == SVG_DIAMOND/.test(lx),
    'a metric label must not look like a button; only the tokens chip opens the dashboard');

  // the dashboard shares the bar's connection, so its close button MUST go through
  // WM_DELETE_WINDOW: without the protocol the window manager falls back to
  // XKillClient, which severs the shared connection and takes the bar with it.
  const dash = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'dash.c'), 'utf8');
  check('linux: the dashboard registers WM_DELETE_WINDOW',
    /XSetWMProtocols\(dpy, d\.win/.test(dash) && /WM_DELETE_WINDOW/.test(dash) &&
    /ev\.xclient\.data\.l\[0\] == d\.wmDelete/.test(dash),
    'a close button that kills the shared connection takes the whole bar down with it');

  // a broken session-bus connection must be RELEASED, or the bar silently loses
  // bar.align 1 for the rest of its life after any KWin/session restart.
  const geom = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'geomdbus.c'), 'utf8');
  check('linux: a dead bus connection is unrefed',
    /closeConn\(/.test(geom) && /return -1;/.test(geom) &&
    !/dbus_connection_unref\(g_conn\); g_conn = NULL;/.test(
      geom.replace(/closeConn\(void\)[\s\S]*?\n\}/, '')),
    'polling an unrefed connection forever is how bar.align 1 dies permanently');
  check('linux: a failed add_match is fatal',
    /dbus_bus_add_match\(g_conn, rule, &err\);\s*\n\s*if \(dbus_error_is_set\(&err\)\) \{[\s\S]*?closeConn\(\);\s*\n\s*return 0;/.test(geom),
    'without the match rule no Report call is ever delivered, so success must not be a lie');

  // the heatmap columns: (back + todayDow)/7 is only Monday-aligned on a Thursday.
  check('linux: the heatmap aligns columns on Monday',
    /weeksBack = \(back \+ 6 - todayDow\) \/ 7/.test(dash),
    'the wrong offset makes the grid read as diagonal streaks on every day but Thursday');
}

// --- the gpu chip: a measured source, never a fabricated one -----------------------
// modules.gpu on an Intel iGPU needs the i915 PMU (perf_event_open on the
// *-busy counters). Three properties must hold: the chip hides rather than
// shows a fake 0% when the source is absent; the PMU permission comes from a
// CAP_PERFMON FILE CAPABILITY on the binary, not from weakening the global
// kernel.perf_event_paranoid; and the percentage is the same quantity Windows
// reports (sum of engine busy fractions, clamped 100).
{
  const pm = fs.readFileSync(path.join(__dirname, '..', 'linux', 'src', 'p_metrics_linux.c'), 'utf8');
  const rd = fs.readFileSync(path.join(__dirname, '..', 'linux', 'README.md'), 'utf8');
  check('linux: gpu hides on no source instead of showing 0%',
    /static int gpuPercent\(void\)/ .test(pm) && /return -1;/.test(pm),
    'a 0%% chip on a GPU with no readable counter is a fabricated reading');
  check('linux: gpu uses the i915 PMU busy counters',
    /perf_event_open/.test(pm) && /rcs0-busy/.test(pm) && /PERF_EVENT_IOC_ENABLE/.test(pm),
    'the engine-busy counter is the one unprivileged-free source that is a real measurement');
  check('linux: gpu permission is a file capability, not a global sysctl',
    /CAP_PERFMON/.test(pm) && /cap_perfmon\+ep/.test(rd) &&
    !/perf_event_paranoid/.test(pm.replace(/kernel\.perf_event_paranoid[^\n]*/g, '')),
    'weakening the whole machine to show one chip would be the wrong trade');
  check('linux: gpu sums engine fractions and clamps like Windows',
    /sum \+= 100\.0 \* \(double\)d \/ \(double\)wall/.test(pm) && /if \(sum > 100\) sum = 100;/.test(pm),
    'the Windows bar sums PDH per-engine utilisation and clamps at 100; the Linux chip must mean the same thing');
  check('linux: gpu counters stay open across ticks',
    /static GpuCounter g_gpuCtr\[GPU_ENGINES_MAX\]/.test(pm) && /g_gpuCtrN < 0\) g_gpuCtrN = gpuProbeI915\(\)/.test(pm),
    're-opening seven PMU events every second would pay the open cost seven times per tick');
}

console.log(failures === 0 ? 'All portable checks passed.' : `FAILURES: ${failures}`);
process.exit(failures === 0 ? 0 : 1);
