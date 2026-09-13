'use strict';
// One-off probe: how does the herdr CLI talk to its server on Windows?
// Lists named pipes and tries both transports for a session.snapshot round-trip.
const fs = require('fs');
const net = require('net');
const path = require('path');

const pipeRoot = '\\\\.\\pipe\\';
const pipes = fs.readdirSync(pipeRoot);
const hits = pipes.filter((p) => /herdr|zai|workspace/i.test(p));
console.log('matching pipes:', hits.length ? hits.join(' | ') : 'none', '(total ' + pipes.length + ')');

const sockPath = path.join(process.env.APPDATA, 'herdr', 'herdr.sock');

function tryConnect(label, opts) {
  return new Promise((resolve) => {
    const s = net.connect(opts);
    let buf = '';
    const done = (verdict) => { try { s.destroy(); } catch (_) {} resolve(verdict); };
    s.setTimeout(5000, () => done(label + ': timeout'));
    s.on('error', (e) => done(label + ': error ' + e.message));
    s.on('connect', () => {
      console.log(label + ': CONNECTED');
      s.write(JSON.stringify({ id: 'probe', method: 'session.snapshot', params: {} }) + '\n');
    });
    s.on('data', (c) => {
      buf += c;
      if (buf.includes('\n')) {
        try {
          const d = JSON.parse(buf.slice(0, buf.indexOf('\n')));
          const ag = (d.result && d.result.snapshot && d.result.snapshot.agents) || [];
          console.log(label + ': response OK, agents =', ag.length);
        } catch (_) { console.log(label + ': unparsable response'); }
        done(label + ': done');
      }
    });
  });
}

(async () => {
  for (const h of hits.slice(0, 8)) {
    const v = await tryConnect('pipe ' + h, { path: pipeRoot + h });
    if (v.includes('response OK')) break;
  }
  await tryConnect('afunix file', { path: sockPath });
})();
