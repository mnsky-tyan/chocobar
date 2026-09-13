// One-shot: live config rescanMinutes 5 -> 1.
const fs = require('fs');
const path = require('path');

const p = path.join(process.env.USERPROFILE, '.wizbar', 'config.json');
const c = JSON.parse(fs.readFileSync(p, 'utf8'));
c.tokens.rescanMinutes = 1;
fs.writeFileSync(p, JSON.stringify(c, null, 2) + '\n');
console.log('rescanMinutes:', c.tokens.rescanMinutes);
