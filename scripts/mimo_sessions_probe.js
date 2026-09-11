// Probe the MiMo desktop API: session list shape (find a cheap "changed" signal).
const fs = require('fs');
const path = require('path');
const cfg = JSON.parse(fs.readFileSync(path.join(process.env.APPDATA, 'Xiaomi MiMo AI', 'desktop-api.json'), 'utf8'));
const H = { Authorization: 'Bearer ' + cfg.token };

fetch(`http://127.0.0.1:${cfg.port}/v1/sessions?limit=100`, { headers: H })
  .then((r) => r.json())
  .then((list) => {
    console.log('sessions:', list.length);
    for (const s of list) {
      console.log(JSON.stringify({ id: s.id, keys: Object.keys(s), titleRevision: s.titleRevision, version: s.version, summary: s.summary }));
    }
    return fetch(`http://127.0.0.1:${cfg.port}/v1/sessions/${list[0].id}/messages`, { headers: H });
  })
  .then((r) => r.json())
  .then((msgs) => {
    console.log('first session messages:', msgs.length);
    const last = msgs[msgs.length - 1];
    console.log('last msg time:', JSON.stringify(last.info.time), 'id:', last.info.id);
    const bytes = JSON.stringify(msgs).length;
    console.log('payload bytes:', bytes);
  })
  .catch((e) => console.error('ERR', e.message));
