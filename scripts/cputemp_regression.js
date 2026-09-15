'use strict';
// Regression for the HWiNFO shared-memory CPU temperature reader.
//
// Run: node scripts/cputemp_regression.js
//
// Builds byte-exact synthetic sections — the legacy layout
// (Global\HWiNFO_SENS_SM, signature 0x10, UTF-16/UTF-8 variants) and the SM2
// layout (Global\HWiNFO_SENS_SM2, signature 'HWiS', the only one HWiNFO 8.24+
// publishes) — and drives the same native.getHwinfoTemp() code path the bar
// uses: open, map, VirtualQuery bound, parse, select. Results are STATE objects
// ({state:'ok'|'no-temp'|'no-section'}), matching what the chip renders. When
// HWiNFO itself is running with shared memory, the last check reads it live.

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

let buildN = 0;
const DEG = '\u00B0C';

// --- legacy layout (signature 0x10) --------------------------------------------
function buildLegacy(name, { wide, readings, sig = 0x10, shift = 0 }) {
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

  const sSize = wide ? 512 : 256, rSize = wide ? 344 : 200;
  enc32(0, sig); enc32(4, 1); enc32(8, 2);
  encF64(16, 1789123456); encF64(24, 987654);
  enc32(32, sSize); enc32(36, rSize);

  const sensors = ['CPU [#0]: AMD Ryzen 9 7950X', 'Motherboard [#1]: ASUS ProArt X670E'];
  let off = 40;
  for (const sn of sensors) {
    wstr(off, sn);
    wstr(off + sSize / 2, sn.startsWith('CPU') ? '\\_SB.SMB0.CPU0' : '\\_SB.PCI0.SMB0');
    off += sSize;
  }
  const base = off + shift * sSize;
  const lU = wide ? 256 : 128, sU = wide ? 32 : 16;
  readings.forEach((r, i) => {
    const ro = base + i * rSize;
    encF64(ro + 16, r.v);
    wstr(ro + 48, r.label);
    wstr(ro + 48 + lU, r.deg ? DEG : 'RPM');
    enc32(ro + 48 + lU + sU, r.s);
  });
  return { h, p };
}

// --- SM2 layout (signature 'HWiS', as published by HWiNFO 8.24+) ----------------
// Header 48B: sig, version, revision, poll time, uptime, header size (48),
// sensor size (392), sensor count, readings offset, reading size (460),
// reading count, capacity. Sensor record: [id u32][flags u32][name]. Reading
// record: [parent u32][label elem][label elem][value elem: [id][unit][cur/min/
// max/avg doubles][label]].
function buildSm2(name, { readings, sig = 0x53695748, sensorCount = 3, cpuParent = 0, withGpu = true }) {
  const MB = 1 << 20;
  const h = CreateFileMappingW((2n ** 64n) - 1n, null, 0x04, 0, MB, name);
  if (!h) throw new Error('CreateFileMappingW failed');
  const p = MapViewOfFile(h, 0x2, 0, 0, 0);
  if (!p) { CloseHandle(h); throw new Error('MapViewOfFile failed'); }
  const enc8 = (o, v) => koffi.encode(p, o, 'uint8', v);
  const enc32 = (o, v) => koffi.encode(p, o, 'uint32', v >>> 0);
  const encF64 = (o, v) => koffi.encode(p, o, 'double', v);
  const wstr = (o, s) => { const b = Buffer.from(s, 'latin1'); for (let i = 0; i < b.length; i++) enc8(o + i, b[i]); };

  const sSize = 392, rSize = 460, hdrSize = 48;
  enc32(0, sig); enc32(4, 2); enc32(8, 1);
  enc32(12, 1789123456); enc32(16, 0);
  enc32(20, hdrSize); enc32(24, sSize); enc32(28, sensorCount);
  const readingsOff = hdrSize + sensorCount * sSize;
  enc32(32, readingsOff); enc32(36, rSize); enc32(40, readings.length); enc32(44, 2000);

  const sensors = ['CPU [#0]: Intel Core Ultra 5 125H', 'Motherboard [#1]: HP 8C31', 'iGPU [#0]: Intel Arc Graphics'];
  sensors.forEach((sn, i) => {
    const so = hdrSize + i * sSize;
    enc32(so, 0xF0000300 + i);
    wstr(so + 8, sn);
  });

  readings.forEach((r, i) => {
    const ro = readingsOff + i * rSize;
    enc32(ro, r.s);
    wstr(ro + 12, r.label);
    wstr(ro + 268, r.deg ? DEG : 'RPM');
    encF64(ro + 284, r.v);
  });
  return { h, p };
}

function teardown(sec) { UnmapViewOfFile(sec.p); CloseHandle(sec.h); }

const coreReadings = [
  { label: 'Core #1 - Temperature', v: 61.25, s: 0, deg: true },
  { label: 'Core #2 - Temperature', v: 63.75, s: 0, deg: true },
  { label: 'CPU (Tctl/Tdie)', v: 64.5, s: 0, deg: true },
  { label: 'CPU Package', v: 65.5, s: 0, deg: true },
  { label: 'GPU Hot Spot', v: 88.5, s: 1, deg: true },
  { label: 'CPU Fan', v: 1200, s: 1, deg: false },
];

for (const wide of [true, false]) {
  const enc = wide ? 'utf16' : 'utf8';

  let sec = buildLegacy(`Local\\WIZBAR_HWI_LEG_${enc}_${process.pid}_${++buildN}`, { wide, readings: coreReadings, shift: 0 });
  let t = native.getHwinfoTemp(`Local\\WIZBAR_HWI_LEG_${enc}_${process.pid}_${buildN}`);
  check(`${enc} legacy: CPU Package selected`, !!t && t.state === 'ok' && t.c === 65.5 && /package/i.test(t.label), t && `${t.c}\u00B0C ${t.label}`);
  teardown(sec);

  sec = buildLegacy(`Local\\WIZBAR_HWI_LEG_${enc}_${process.pid}_${++buildN}`, { wide, readings: coreReadings.filter((r) => !/package/i.test(r.label)), shift: 0 });
  t = native.getHwinfoTemp(`Local\\WIZBAR_HWI_LEG_${enc}_${process.pid}_${buildN}`);
  check(`${enc} legacy: Tctl fallback`, !!t && t.state === 'ok' && t.c === 64.5 && /tctl/i.test(t.label), t && `${t.c}\u00B0C ${t.label}`);
  teardown(sec);

  sec = buildLegacy(`Local\\WIZBAR_HWI_LEG_${enc}_${process.pid}_${++buildN}`, { wide, readings: coreReadings.filter((r) => r.s === 0 && !/package|tctl/i.test(r.label)), shift: 0 });
  t = native.getHwinfoTemp(`Local\\WIZBAR_HWI_LEG_${enc}_${process.pid}_${buildN}`);
  check(`${enc} legacy: hottest core fallback`, !!t && t.state === 'ok' && t.c === 63.8 && /core/i.test(t.label), t && `${t.c}\u00B0C ${t.label}`);
  teardown(sec);

  sec = buildLegacy(`Local\\WIZBAR_HWI_LEG_${enc}_${process.pid}_${++buildN}`, { wide, readings: coreReadings.filter((r) => r.s === 1), shift: 0 });
  t = native.getHwinfoTemp(`Local\\WIZBAR_HWI_LEG_${enc}_${process.pid}_${buildN}`);
  check(`${enc} legacy: non-CPU sensors -> no-temp`, !!t && t.state === 'no-temp', t && t.state);
  teardown(sec);

  sec = buildLegacy(`Local\\WIZBAR_HWI_LEG_${enc}_${process.pid}_${++buildN}`, { wide, readings: coreReadings, sig: 0x99, shift: 0 });
  t = native.getHwinfoTemp(`Local\\WIZBAR_HWI_LEG_${enc}_${process.pid}_${buildN}`);
  check(`${enc} legacy: bad signature -> no-section`, !!t && t.state === 'no-section', t && t.state);
  teardown(sec);
}

// SM2 scenarios (the layout HWiNFO 8.52 actually publishes)
{
  const readings = [
    { label: 'Core 0', v: 61, s: 0, deg: true },
    { label: 'Core 1', v: 62, s: 0, deg: true },
    { label: 'CPU Package', v: 66, s: 0, deg: true },
    { label: 'GPU Core Temperature', v: 87, s: 2, deg: true },
    { label: 'CPU Fan', v: 1200, s: 1, deg: false },
  ];
  const sec = buildSm2(`Local\\WIZBAR_HWI_SM2_${process.pid}`, { readings });
  const t = native.getHwinfoTemp(`Local\\WIZBAR_HWI_SM2_${process.pid}`);
  check('SM2: CPU Package selected', !!t && t.state === 'ok' && t.c === 66 && /package/i.test(t.label), t && `${t.c}\u00B0C ${t.label}`);
  teardown(sec);
}
{
  // per-core temps only: hottest core wins, Distance-to-TjMAX never misattributed
  const readings = [
    { label: 'E-core 0', v: 71, s: 0, deg: true },
    { label: 'P-core 8', v: 74, s: 0, deg: true },
    { label: 'E-core 0 Distance to TjMAX', v: 24, s: 0, deg: true },
    { label: 'P-core 8 Distance to TjMAX', v: 16, s: 0, deg: true },
  ];
  const sec = buildSm2(`Local\\WIZBAR_HWI_SM2_CORES_${process.pid}`, { readings });
  const t = native.getHwinfoTemp(`Local\\WIZBAR_HWI_SM2_CORES_${process.pid}`);
  check('SM2: hottest core fallback (Distance excluded)', !!t && t.state === 'ok' && t.c === 74, t && `${t.c}\u00B0C ${t.label}`);
  teardown(sec);
}
{
  // only non-CPU temps: valid section, nothing to report
  const readings = [{ label: 'GPU Core Temperature', v: 87, s: 2, deg: true }];
  const sec = buildSm2(`Local\\WIZBAR_HWI_SM2_NOGPU_${process.pid}`, { readings });
  const t = native.getHwinfoTemp(`Local\\WIZBAR_HWI_SM2_NOGPU_${process.pid}`);
  check('SM2: non-CPU temps -> no-temp', !!t && t.state === 'no-temp', t && t.state);
  teardown(sec);
}
{
  const sec = buildSm2(`Local\\WIZBAR_HWI_SM2_BAD_${process.pid}`, { readings: [{ label: 'CPU Package', v: 66, s: 0, deg: true }], sig: 0x12345678 });
  const t = native.getHwinfoTemp(`Local\\WIZBAR_HWI_SM2_BAD_${process.pid}`);
  check('SM2: bad signature -> no-section', !!t && t.state === 'no-section', t && t.state);
  teardown(sec);
}

// absent section: the graceful path the bar lives on without HWiNFO
const tAbsent = native.getHwinfoTemp(`Local\\WIZBAR_HWI_NOPE_${process.pid}`);
check('absent section -> no-section (graceful)', !!tAbsent && tAbsent.state === 'no-section', tAbsent && tAbsent.state);

// live HWiNFO, if it is running with shared memory right now
const probeOpen = kernel32.func('uintptr_t __stdcall OpenFileMappingW(uint32_t access, int inherit, str16 name)');
const liveSM2 = probeOpen(0x4, 0, 'Global\\HWiNFO_SENS_SM2');
const liveSM1 = probeOpen(0x4, 0, 'Global\\HWiNFO_SENS_SM');
if (liveSM2 || liveSM1) {
  if (liveSM2) CloseHandle(liveSM2);
  if (liveSM1) CloseHandle(liveSM1);
  const t = native.getHwinfoTemp();
  check('live HWiNFO section parses', !!t && t.state === 'ok' && t.c > 0 && t.c < 150, t && `${t.c}\u00B0C ${t.label} (${t.state})`);
} else {
  console.log('NOTE: no live HWiNFO shared memory right now (HWiNFO not running or shm off).');
}

console.log(failures ? `\n${failures} FAILURE(S)` : '\nALL CHECKS PASSED');
process.exit(failures ? 1 : 0);
