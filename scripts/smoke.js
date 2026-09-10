'use strict';
// Smoke tests for the non-Electron pieces.
process.chdir(__dirname + '/..');

async function main() {
  console.log('--- native ---');
  const native = require('../src/native');
  const wins = native.listWindowsByClass('CASCADIA_HOSTING_WINDOW_CLASS');
  console.log('terminal windows found:', wins.length, wins.map((w) => w.toString(16)));
  if (wins.length) {
    console.log('rect:', native.getWindowRect(wins[0]));
    console.log('iconic:', native.isIconic(wins[0]));
  }
  console.log('battery:', native.getBattery());
  const ok = native.initVolume('multimedia');
  console.log('volume init:', ok, native.volumeState.error || '');
  if (ok) console.log('volume:', native.getVolume());

  console.log('--- tokens ---');
  const { ConfigManager } = require('../src/config');
  const cm = new ConfigManager();
  const cfg = cm.load();
  const { TokenTracker } = require('../src/tokens');
  const tt = new TokenTracker(cfg);
  const n1 = tt._scanZcode();
  const n2 = tt._scanOpencode();
  console.log(`scan: zcode +${n1}, opencode +${n2}, total ${tt.records.size}`);
  const agg = tt.aggregate();
  console.log('today:', agg.today.total, 'week:', agg.week, 'allTime:', agg.allTime);
  console.log('byApp:', JSON.stringify(agg.byApp, null, 1).slice(0, 600));
  const days = Object.keys(agg.byDay);
  console.log('days:', days.length, days.slice(0, 3), '...', days.slice(-3));
  process.exit(0);
}

main().catch((e) => { console.error('SMOKE FAILED:', e); process.exit(1); });
