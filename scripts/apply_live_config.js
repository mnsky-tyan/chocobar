// One-shot: update the live ~/.wizbar/config.json (tray off, mimo source, pet module).
const fs = require('fs');
const path = require('path');

const p = path.join(process.env.USERPROFILE, '.wizbar', 'config.json');
const c = JSON.parse(fs.readFileSync(p, 'utf8'));
c.general.showTray = false;
c.modules.pet = {
  enabled: true,
  exePath: '' // point at your own pet executable
};
c.tokens.sources.mimo = { enabled: true };
fs.writeFileSync(p, JSON.stringify(c, null, 2) + '\n');
console.log('written. pet exe exists on disk:', fs.existsSync(c.modules.pet.exePath));
console.log('showTray:', c.general.showTray, '| mimo:', JSON.stringify(c.tokens.sources.mimo));
