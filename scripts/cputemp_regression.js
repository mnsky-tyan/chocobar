'use strict';
// Regression for the HWiNFO shared-memory CPU temperature reader.
//
// Run: node scripts/cputemp_regression.js
//
// Builds byte-exact synthetic HWiNFO sections (UTF-16 and UTF-8 revisions, both
// readings-array placements the SDK leaves ambiguous), publishes them as real
// named sections, and drives the same native.getHwinfoTemp() code path the bar
// uses — open, map, VirtualQuery bound, parse, select. HWiNFO itself is not
// required; when it IS running with shared memory, the last check reads the
// live section through the unmodified default path.

const koffi = require('koffi');
const native = require('../src/native');

const kernel32 = koffi.load('kernel32.dll');
const CreateFileMappingW = kernel32.func('uintptr_t __stdcall CreateFileMappingW(uintptr_t hFile, void *attrs, uint32_t prot, uint32_t maxHi, uint32_t maxLo, str16 name)');
const MapViewOfFile = kernel32.func('void *__stdcall MapViewOfFile(uintptr_t h, uint32_t access, uint32_t hi, uint32_t lo, size_t bytes)');
const UnmapViewOfFile = kernel32.func('int __stdcall UnmapViewOfFile(void *p)');
const CloseHandle = kernel32.func('int __stdcall CloseHandle(uintptr_t h)');

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}${detail ? '  [' + detail + ']' : ''}`);
  if (!ok) failures++;
}

const HEADER = 40;
const LABEL_AT = 48;
const SUFFIX_UNITS = wide => (wide ? 32 : 16);
const LABEL_UNITS = wide => SUFFIX_UNITS(wide) * 8;
const SENSOR_SIZE = wide => (wide ? 512 : 256);
const READING_SIZE = wide => (wide ? 344 : 200);

// A synthetic section: two sensors, then readings laid out at `shift` sensor
// elements past the sensor terminator (0 and 1 = the two SDK placements).
let buildN = 0;
function buildSection(_unused, { wide, readings, sig = 0x10, shift = 0 }) {
  const name = `Local\WIZBAR_HWI_${process.pid}_${++buildN}`;
  const MB = 1 << 20;
  const h = CreateFileMappingW((2n ** 64n) - 1n, null, 0x04, 0, MB, name);
  if (!h) throw new Error('CreateFileMappingW failed');
  const p = MapViewOfFile(h, 0x2, 0, 0, 0);
  if (!p) { CloseHandle(h); throw new Error('MapViewOfFile failed'); }
  const enc8 = (o, v) => koffi.encode(p, o, 'uint8', v);
  const enc32 = (o, v) => koffi.encode(p, o, 'uint32', v >>> 0);
  const encF64 = (o, v) => koffi.encode(p, o, 'double', v);
  const wstr = (o, s) => {
    const b = Buffer.from(s, wide ? 'utf16le' : 'utf8');
    for (let i = 0; i < b.length; i++) enc8(o + i, b[i]);
  };

  const sSize = SENSOR_SIZE(wide), rSize = READING_SIZE(wide);
  enc32(0, sig); enc32(4, 1); enc32(8, 2);
  encF64(16, 1789123456); encF64(24, 987654);
  enc32(32, sSize); enc32(36, rSize);

  const sensors = ['CPU [#0]: AMD Ryzen 9 7950X', 'Motherboard [#1]: ASUS ProArt X670E'];
  let off = HEADER;
  for (const sn of sensors) {
    wstr(off, sn);
    wstr(off + sSize / 2, sn.startsWith('CPU') ? '\\_SB.SMB0.CPU0' : '\\_SB.PCI0.SMB0');
    off += sSize;
  }
  // off = terminator slot; readings go `shift` sensor elements later
  const base = off + shift * sSize;
  const lU = LABEL_UNITS(wide), sU = SUFFIX_UNITS(wide);
  readings.forEach((r, i) => {
    const ro = base + i * rSize;
    encF64(ro + 16, r.v);
    wstr(ro + LABEL_AT, r.label);
    wstr(ro + LABEL_AT + lU, r.deg ? '°C' : 'RPM');
    enc32(ro + LABEL_AT + lU + sU, r.s);
  });
  return { h, p, name };
}

function teardown(sec) { UnmapViewOfFile(sec.p); CloseHandle(sec.h); }

// --- selection scenarios --------------------------------------------------------
const coreReadings = [
  { label: 'Core #1 - Temperature', v: 61.25, s: 0, deg: true },
  { label: 'Core #2 - Temperature', v: 63.75, s: 0, deg: true },
  { label: 'CPU (Tctl/Tdie)', v: 64.5, s: 0, deg: true },
  { label: 'CPU Package', v: 65.5, s: 0, deg: true },
  { label: 'GPU Hot Spot', v: 88.5, s: 1, deg: true },   // other sensor: never CPU
  { label: 'CPU Fan', v: 1200, s: 1, deg: false },        // RPM: no ° suffix, skipped
];
const suffix0 = coreReadings.concat([
  { label: 'Motherboard Temperature', v: 34.0, s: 1, deg: true },
]);

let t; // hoisted: the absent/live checks after the loop reuse it
let scenario = 0;
for (const wide of [true, false]) {
  const enc = wide ? 'utf16' : 'utf8';
  // unique name per scenario: sections with a reused name would keep stale bytes
  const name = `Local\\WIZBAR_HWI_TEST_${enc}_${process.pid}_${++scenario}`;

  // readings on the terminator slot placement
  let sec = buildSection(name, { wide, readings: suffix0, shift: 0 });
  t = native.getHwinfoTemp(sec.name);
  check(`${enc}: CPU Package selected (terminator-slot readings)`, !!t && t.c === 65.5 && /package/i.test(t.label), t && `${t.c}°C ${t.label}`);
  teardown(sec);

  // readings one slot later (the other SDK placement)
  sec = buildSection(name, { wide, readings: suffix0, shift: 1 });
  t = native.getHwinfoTemp(sec.name);
  check(`${enc}: CPU Package selected (post-terminator readings)`, !!t && t.c === 65.5, t && `${t.c}°C ${t.label}`);
  teardown(sec);

  // Tctl fallback when no Package reading exists
  sec = buildSection(name, { wide, readings: suffix0.filter((r) => !/package/i.test(r.label)), shift: 0 });
  t = native.getHwinfoTemp(sec.name);
  check(`${enc}: Tctl fallback`, !!t && t.c === 64.5 && /tctl/i.test(t.label), t && `${t.c}°C ${t.label}`);
  teardown(sec);

  // hottest core when only per-core temps exist on the CPU sensor
  sec = buildSection(name, {
    wide,
    readings: suffix0.filter((r) => r.s === 0 && !/package|tctl/i.test(r.label)),
    shift: 0,
  });
  t = native.getHwinfoTemp(sec.name);
  check(`${enc}: hottest core fallback`, !!t && t.c === 63.8 && /core/i.test(t.label), t && `${t.c}°C ${t.label}`);
  teardown(sec);

  // temps only on a non-CPU sensor must NOT be reported as CPU
  sec = buildSection(name, {
    wide,
    readings: suffix0.filter((r) => r.s === 1),
    shift: 0,
  });
  t = native.getHwinfoTemp(sec.name);
  check(`${enc}: non-CPU sensors ignored`, t === null, t && `${t.c}°C ${t.label}`);
  teardown(sec);

  // wrong signature: HWiNFO restarting / garbage map
  sec = buildSection(name, { wide, readings: suffix0, sig: 0x99, shift: 0 });
  t = native.getHwinfoTemp(sec.name);
  check(`${enc}: bad signature rejected`, t === null);
  teardown(sec);
}

// absent section: the graceful path the bar lives on without HWiNFO
t = native.getHwinfoTemp(`Local\\WIZBAR_HWI_NOPE_${process.pid}`);
check('absent section returns null (graceful)', t === null);

// live HWiNFO, if it is running with shared memory right now
const koffi2 = koffi;
const kernel32b = koffi2.load('kernel32.dll');
const probeOpen = kernel32b.func('uintptr_t __stdcall OpenFileMappingW(uint32_t access, int inherit, str16 name)');
const liveH = probeOpen(0x4, 0, 'Global\\HWiNFO_SENS_SM');
if (liveH) {
  CloseHandle(liveH);
  t = native.getHwinfoTemp();
  check('live HWiNFO section parses', !!t && t.c > 0 && t.c < 150, t && `${t.c}°C ${t.label} (${t.sensor})`);
} else {
  console.log('NOTE: HWiNFO shared memory not live — enable "Shared Memory Support" in HWiNFO to activate the bar chip.');
}

console.log(failures ? `\n${failures} FAILURE(S)` : '\nALL CHECKS PASSED');
process.exit(failures ? 1 : 0);
