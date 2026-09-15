// Analyze a fetched MiMo messages dump: roles + look for usage/token fields.
const fs = require('fs');
const p = process.argv[2];
const m = JSON.parse(fs.readFileSync(p, 'utf8'));
console.log('count', m.length);
const roles = {};
for (const x of m) roles[x.info.role] = (roles[x.info.role] || 0) + 1;
console.log(roles);
for (const x of m) {
  if (x.info.role === 'assistant') {
    console.log('ASSISTANT INFO KEYS:', Object.keys(x.info));
    console.log('ASSISTANT INFO:', JSON.stringify(x.info, (k, v) => (typeof v === 'string' && v.length > 200) ? v.slice(0, 200) + '...' : v).slice(0, 1500));
    break;
  }
}
// any key containing usage/token anywhere in any message info
const hits = new Set();
const walk = (o, path) => {
  if (!o || typeof o !== 'object') return;
  for (const [k, v] of Object.entries(o)) {
    if (/usage|token/i.test(k)) hits.add(path + '.' + k + ' = ' + JSON.stringify(v).slice(0, 120));
    if (typeof v === 'object') walk(v, path + '.' + k);
  }
};
m.forEach((x, i) => walk(x.info, 'm[' + i + '].info'));
console.log('USAGE FIELD HITS:', [...hits].slice(0, 10));
