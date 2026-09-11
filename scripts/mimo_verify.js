// Aggregate check: recompute per-app totals from the token cache the way
// src/tokens.js does, printing the mimo row for eyeball verification.
const fs = require('fs');
const path = require('path');
const c = JSON.parse(fs.readFileSync(path.join(process.env.USERPROFILE, '.wizbar', 'token-cache.json'), 'utf8'));
const agg = {};
for (const [, r] of c.entries) {
  if (!agg[r.app]) agg[r.app] = { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, requests: 0 };
  const a = agg[r.app];
  a.input += r.input || 0; a.output += r.output || 0;
  a.cacheRead += r.cacheRead || 0; a.cacheWrite += r.cacheWrite || 0;
  a.requests += 1;
}
for (const [app, a] of Object.entries(agg)) {
  console.log(app.padEnd(10),
    'in=' + fmt(a.input), 'out=' + fmt(a.output),
    'cacheR=' + fmt(a.cacheRead), 'cacheW=' + fmt(a.cacheWrite),
    'calls=' + a.requests, 'total=' + fmt(a.input + a.output));
}
function fmt(n) {
  if (n >= 1e9) return (n / 1e9).toFixed(2) + 'B';
  if (n >= 1e6) return (n / 1e6).toFixed(2) + 'M';
  if (n >= 1e3) return (n / 1e3).toFixed(1) + 'k';
  return String(n);
}
// spot-check one mimo record against raw API numbers (hand-verified earlier:
// input 4586 + cache.read 36864 = 41450, output 192)
const sample = c.entries.find(([k, v]) => v.app === 'mimo');
console.log('mimo sample record:', JSON.stringify(sample));
