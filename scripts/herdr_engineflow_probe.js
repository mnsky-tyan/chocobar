'use strict';
// Isolate why the engine's herdr socket gets EPIPE while the plain probe worked.
const net = require('net');
const path = require('path');

const prefix = '\\\\.\\pipe\\';
const full = prefix + path.join(process.env.APPDATA, 'herdr', 'herdr.sock');

function t(label, { encoding = false, msgs = [], delayMs = 0 }) {
  return new Promise((res) => {
    const s = net.connect({ path: full });
    if (encoding) s.setEncoding('utf8');
    let buf = '';
    s.setTimeout(4000);
    s.on('connect', () => {
      console.log(label, 'connected');
      const send = () => {
        for (const m of msgs) {
          console.log(label, 'writing:', m.slice(0, 60) + '...');
          s.write(m + '\n');
        }
      };
      if (delayMs) setTimeout(send, delayMs);
      else send();
    });
    s.on('data', (c) => {
      buf += c;
      const lines = buf.split('\n').filter(Boolean);
      console.log(label, 'data, lines:', lines.length, 'first-id:', (() => { try { return JSON.parse(lines[0]).id; } catch { return '?'; } })());
    });
    s.on('close', () => { console.log(label, 'CLOSED'); res(); });
    s.on('error', (e) => { console.log(label, 'ERROR', e.message); });
    s.on('timeout', () => { console.log(label, 'TIMEOUT'); s.destroy(); res(); });
  });
}

const snap = JSON.stringify({ id: 'snap1', method: 'session.snapshot', params: {} });
const sub = JSON.stringify({
  id: 'sub-unscoped',
  method: 'events.subscribe',
  params: { subscriptions: [{ type: 'pane.agent_detected' }, { type: 'pane.created' }] }
});

(async () => {
  await t('A one-write           ', { msgs: [snap] });
  await t('B two-write           ', { msgs: [snap, sub] });
  await t('C setEncoding two-write', { encoding: true, msgs: [snap, sub] });
  await t('D sub first           ', { msgs: [sub, snap] });
  process.exit(0);
})();
