'use strict';
// Bisect the herdr socket handshake: which message sequence makes the server close?
const net = require('net');
const path = require('path');

const prefix = '\\\\.\\pipe\\';
const full = prefix + path.join(process.env.APPDATA, 'herdr', 'herdr.sock');

function t(label, msgs) {
  return new Promise((res) => {
    const s = net.connect({ path: full });
    let buf = '';
    s.setTimeout(4000);
    s.on('connect', () => { for (const m of msgs) s.write(m + '\n'); });
    s.on('data', (c) => {
      buf += c;
      const lines = buf.split('\n').filter(Boolean);
      const ids = lines.map((l) => {
        try { const d = JSON.parse(l); return d.id || d.type || '?'; } catch { return 'unparsed'; }
      }).join(',');
      console.log(label, '-> lines:', lines.length, 'ids:', ids);
      s.destroy();
      res();
    });
    s.on('timeout', () => {
      console.log(label, '-> TIMEOUT, lines so far:', buf.split('\n').filter(Boolean).length);
      s.destroy();
      res();
    });
    s.on('error', (e) => { console.log(label, '-> ERR', e.message); res(); });
    s.on('close', () => { console.log(label, '-> (closed)'); });
  });
}

const snap = JSON.stringify({ id: 'snap1', method: 'session.snapshot', params: {} });
const sub = JSON.stringify({
  id: 'sub1',
  method: 'events.subscribe',
  params: { subscriptions: [{ type: 'pane.agent_status_changed' }] }
});

(async () => {
  await t('snap only     ', [snap]);
  await t('snap then sub ', [snap, sub]);
  await t('sub only      ', [sub]);
  process.exit(0);
})();
