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
    .map((s) => s.replace(/\\(.)/g, (_m, ch) => ({ n: '\n', r: '\r', t: '\t', '"': '"', '\\': '\\' }[ch] || ch)))
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
    check('template: no personal identifiers in the template',
      !/tyanw|mnsky|firstmate/i.test(decode(m[1])));
  }
}

console.log(failures === 0 ? 'All portable checks passed.' : `FAILURES: ${failures}`);
process.exit(failures === 0 ? 0 : 1);
