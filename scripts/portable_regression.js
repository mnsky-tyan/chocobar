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
  const c = fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'p_ui.c'), 'utf8');
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
    // the estimator of blind turns: an estimate is not a measurement, so it ships
    // OFF and says so, and its constants stay overridable in the config file
    // rather than being baked into the source
    check('template: estimateMissingUsage on by default',
      tok.estimateMissingUsage === true);
    check('template: estimate block absent by default (built-ins apply)',
      !('estimate' in tok));
    check('template: no personal identifiers in the template',
      !/tyanw|mnsky|firstmate|captain|crewmate|remielle/i.test(decode(m[1])));
  }
}

// --- C-side estimator guards --------------------------------------------------
// The estimator (native/src/p_tokens.c) decides WHICH lines are counted and how,
// and the config parser (native/src/chocobar.c) decides the constants. Both are
// easy to break in a way that only shows up as a slightly wrong number months
// later, so pin the properties that make the estimate honest.
{
  const pt = fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'p_tokens.c'), 'utf8');
  const cb = fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'chocobar.c'), 'utf8');

  // a bridge turn must be judged by its route, not by a hardcoded provider list:
  // mimo rows have been toggled off and on, and workbuddy-2 appeared later, so any
  // such list is wrong the day a route changes. The code consults the set of
  // routes that DID report in the same file, and no route name is compiled in.
  check('estimator: blindness is decided from the file\'s own reporting routes',
    /strstr\(real, pv\)/.test(pt) && /blind = 0/.test(pt));
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

  // off by default, with the constants in config and a sane floor
  check('config: estimateMissingUsage defaults to on',
    /tokensEnabled = jboolDefault\(js, t, jobjGet\(js, t, toks, "enabled"\), 1\);[\s\S]{0,700}tokEstimate = jboolDefault\(js, t, jobjGet\(js, t, toks, "estimateMissingUsage"\), 1\);/.test(cb));
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
  const ui = fs.readFileSync(path.join(__dirname, '..', 'native', 'src', 'p_ui.c'), 'utf8');
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

console.log(failures === 0 ? 'All portable checks passed.' : `FAILURES: ${failures}`);
process.exit(failures === 0 ? 0 : 1);
