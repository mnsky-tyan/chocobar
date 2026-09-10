'use strict';
// Bar renderer: builds segments right-aligned, updates on pushed stats.

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

const ICONS = {
  cpu: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><rect x="6" y="6" width="12" height="12" rx="1.5"/><path d="M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3"/></svg>`,
  temp: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M10 4a2 2 0 1 1 4 0v9.3a4.5 4.5 0 1 1-4 0z"/><path d="M12 9.5v6.5"/></svg>`,
  gpu: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="7" width="16" height="10" rx="1.5"/><circle cx="9" cy="12" r="2.4"/><path d="M14 9.5v5M17 9.5v5M19 10v4M6 17v3M10 17v3"/></svg>`,
  ram: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="8" width="18" height="9" rx="1.5"/><path d="M7 17v3M12 17v3M17 17v3M7 11v3M11 11v3M15 11v3"/></svg>`,
  vol: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 5 6.5 9H3v6h3.5L11 19V5z"/><path d="M15.5 9.5a4 4 0 0 1 0 5M18 7a7.5 7.5 0 0 1 0 10"/></svg>`,
  mute: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 5 6.5 9H3v6h3.5L11 19V5z"/><path d="m16 9.5 5 5M21 9.5l-5 5"/></svg>`,
  bat: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><rect x="2.5" y="8" width="16" height="8.5" rx="1.5"/><path d="M21.5 11v2.5"/><path d="M11 9.6l-2.4 3.2h2.8L9 16"/></svg>`,
  batCharge: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><rect x="2.5" y="8" width="16" height="8.5" rx="1.5"/><path d="M21.5 11v2.5"/><path d="M11.5 9.2 8.6 13h2.6l-2.4 3.6"/></svg>`,
  batPlug: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><rect x="2.5" y="8" width="16" height="8.5" rx="1.5"/><path d="M21.5 11v2.5"/><path d="M6 10.8v3.4M8.5 10.8v3.4"/></svg>`,
  // Battery body whose fill volume tracks the current percentage. fill ranges
  // from a barely-visible sliver at <=10% to full at 100%; below 10% the fill
  // turns red (theme warn) so a dying battery reads at a glance.
  batBody(pct, charging) {
    const p = Math.max(0, Math.min(100, pct ?? 0));
    const innerX = 4.3, innerW = 12.4, y = 9.7, h = 5.1;
    // never zero-width: 4% floor keeps a sliver visible even near dead
    const w = Math.max(0.9, (p / 100) * innerW);
    const low = p < 10;
    const fill = low ? 'var(--warn)' : 'currentColor';
    const bolt = charging
      ? '<path d="M12.2 8.9 9.6 12.4h2.4l-2.1 3.3" fill="none" stroke-width="1.8"/>'
      : '';
    return `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">` +
      `<rect x="2.5" y="8" width="16" height="8.5" rx="1.5"/><path d="M21.5 11v2.5"/>` +
      `<rect x="${innerX.toFixed(1)}" y="${y}" width="${w.toFixed(2)}" height="${h}" rx="0.8" fill="${fill}" stroke="none" opacity="${low ? 0.9 : 0.75}"/>` +
      bolt + `</svg>`;
  },
  buds: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M8.5 3a3 3 0 0 0-3 3v2.2a3 3 0 1 0 3 3V3z"/><path d="M8.5 13.5v2a3.5 3.5 0 0 1-3.4 3.5"/><path d="M15.5 3a3 3 0 0 1 3 3v2.2a3 3 0 1 1-3 3V3z"/><path d="M15.5 13.5v2a3.5 3.5 0 0 0 3.4 3.5"/></svg>`,
  clock: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="8.5"/><path d="M12 7.5V12l3 2"/></svg>`,
  diamond: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linejoin="round"><path d="M12 3 21 12 12 21 3 12z"/></svg>`
};

let theme = null;
let stats = null;
let tokensAgg = null;
let segEls = {};

function el(id) { return document.getElementById(id); }

function applyTheme(t) {
  theme = t;
  const r = document.documentElement.style;
  const map = {
    '--fg': t.fg, '--fg-dim': t.fgDim, '--pink': t.pink, '--pink-deep': t.pinkDeep,
    '--pink-bg': t.pinkBg, '--yellow': t.yellow, '--yellow-bg': t.yellowBg,
    '--warn': t.warn, '--good': t.good, '--divider': t.divider
  };
  for (const [k, v] of Object.entries(map)) if (v) r.setProperty(k, v);
  if (t.bar) {
    if (t.bar.bgCss) r.setProperty('--bg', t.bar.bgCss);
    if (t.bar.fontSize) r.setProperty('--fs', t.bar.fontSize + 'px');
    if (t.bar.fontFamily) r.setProperty('--font', t.bar.fontFamily);
    if (t.bar.radius !== undefined) r.setProperty('--radius', t.bar.radius + 'px');
    if (t.bar.segmentSpacing) r.setProperty('--seg-gap', t.bar.segmentSpacing + 'px');
    const bar = el('bar');
    bar.classList.remove('align-left', 'align-center');
    if (t.bar.align === 'left') bar.classList.add('align-left');
    else if (t.bar.align === 'center') bar.classList.add('align-center');
  }
  rebuildSegments();
}

function seg(id, iconSvg, clickable) {
  const s = document.createElement('div');
  s.className = 'seg' + (clickable ? ' clickable' : '');
  s.id = 'seg-' + id;
  const ico = document.createElement('span');
  ico.className = 'ico';
  ico.innerHTML = iconSvg;
  const val = document.createElement('span');
  val.className = 'val';
  s.append(ico, val);
  segEls[id] = { root: s, ico, val };
  return s;
}

function rebuildSegments() {
  const c = el('segments');
  // Pinned token chips are direct children of #bar — remove stale ones from
  // earlier rebuilds before appending a fresh chip.
  for (const n of [...el('bar').querySelectorAll(':scope > #seg-tokens')]) n.remove();
  c.innerHTML = '';
  segEls = {};
  if (!theme) return;
  const m = theme.modules || {};

  if (theme.tokens && theme.tokens.showOnBar) {
    const s = seg('tokens', ICONS.diamond, true);
    s.title = 'Token usage today — click to open dashboard';
    s.addEventListener('click', () => window.wizbar.openDash());
    // With the default right-aligned group, pin the token chip to the far left
    // of the bar: it must be a direct child of #bar, inserted BEFORE #segments,
    // and its margin-right:auto eats all free space so the group stays right.
    if ((theme.bar.align || 'right') === 'right') el('bar').insertBefore(s, el('segments'));
    else c.appendChild(s);
  }
  const titles = { gpu: 'GPU usage', cpu: 'CPU usage', cputemp: 'CPU temperature (HWiNFO)', ram: 'Memory usage', volume: 'Volume', battery: 'Battery', bluetooth: 'Bluetooth device battery', clock: 'Local time' };
  for (const [id, icon] of [['gpu', ICONS.gpu], ['cpu', ICONS.cpu], ['cputemp', ICONS.temp], ['ram', ICONS.ram], ['volume', ICONS.vol], ['battery', ICONS.bat], ['bluetooth', ICONS.buds], ['clock', ICONS.clock]]) {
    if (m[id] && m[id].enabled) {
      const s = seg(id, icon);
      s.title = titles[id];
      c.appendChild(s);
    }
  }

  render();
}

document.body.addEventListener('contextmenu', (e) => {
  e.preventDefault();
  window.wizbar.contextMenu();
});

function setVal(id, text, cls) {
  const s = segEls[id];
  if (!s) return;
  s.val.textContent = text;
  s.val.className = 'val' + (cls ? ' ' + cls : '');
}

function setIcon(id, svg) {
  const s = segEls[id];
  if (s && s.ico.innerHTML !== svg) s.ico.innerHTML = svg;
}

function fmtTokens(n) {
  if (n == null) return '—';
  if (n >= 1e9) return (n / 1e9).toFixed(2) + 'B';
  if (n >= 1e6) return (n / 1e6).toFixed(1) + 'M';
  if (n >= 1e3) return (n / 1e3).toFixed(1) + 'k';
  return String(n);
}

function fmtClock(fmt, d) {
  const p2 = (x) => String(x).padStart(2, '0');
  return (fmt || '{MMM} {dd}  {HH}:{mm}')
    .replace('{MMM}', MONTHS[d.getMonth()])
    .replace('{mmmm}', MONTHS[d.getMonth()].toUpperCase())
    .replace('{dd}', p2(d.getDate()))
    .replace('{d}', String(d.getDate()))
    .replace('{HH}', p2(d.getHours()))
    .replace('{H}', String(d.getHours()))
    .replace('{mm}', p2(d.getMinutes()))
    .replace('{ss}', p2(d.getSeconds()))
    .replace('{yy}', String(d.getFullYear() % 100));
}

function render() {
  if (!theme) return;
  const m = theme.modules || {};

  if (segEls.tokens) setVal('tokens', fmtTokens(tokensAgg ? tokensAgg.today.total : null), 'dim');

  if (segEls.gpu) {
    if (stats && stats.gpu && !stats.gpu.error) {
      setVal('gpu', (stats.gpu[theme.modules.gpu.mode === 'max' ? 'max' : 'sum'] ?? stats.gpu.sum) + '%',
        (stats.gpu.sum ?? 0) >= 85 ? 'warn' : '');
    } else setVal('gpu', '—', 'dim');
  }
  if (segEls.cpu) {
    setVal('cpu', stats && stats.cpu != null ? stats.cpu + '%' : '—',
      stats && m.cpu.warnAt && stats.cpu >= m.cpu.warnAt ? 'warn' : '');
  }
  if (segEls.cputemp) {
    const t = stats && stats.cpuTemp;
    if (t && t.state === 'ok' && t.c != null) {
      setVal('cputemp', (t.c % 1 ? t.c.toFixed(1) : t.c.toFixed(0)) + '°C',
        m.cputemp && m.cputemp.warnAt && t.c >= m.cputemp.warnAt ? 'warn' : '');
      segEls.cputemp.root.title = t.label ? `CPU temperature — ${t.label}` : 'CPU temperature';
    } else {
      setVal('cputemp', '—', 'dim');
      segEls.cputemp.root.title = t && t.state === 'no-temp'
        ? 'CPU temperature — HWiNFO sensors are live but report no CPU temperature'
        : 'CPU temperature — HWiNFO not running (or Shared Memory Support off)';
    }
  }
  if (segEls.ram) {
    setVal('ram', stats && stats.ram ? stats.ram.pct + '%' : '—',
      stats && m.ram.warnAt && stats.ram && stats.ram.pct >= m.ram.warnAt ? 'warn' : '');
    if (stats && stats.ram) segEls.ram.root.title = `${stats.ram.usedGB} GB / ${stats.ram.totalGB} GB`;
  }
  if (segEls.volume) {
    if (stats && stats.volume) {
      setIcon('volume', stats.volume.muted ? ICONS.mute : ICONS.vol);
      setVal('volume', stats.volume.level + '%', stats.volume.muted ? 'dim' : '');
    } else setVal('volume', '—', 'dim');
  }
  if (segEls.battery) {
    const b = stats && stats.battery;
    if (b && b.percent != null && !b.noBattery) {
      // One proportional icon: fill volume = charge, red below 10%, bolt while
      // charging. "Plugged at 100%" simply renders full with no bolt.
      setIcon('battery', ICONS.batBody(b.percent, b.charging));
      setVal('battery', b.percent + '%', b.percent <= 20 && !b.ac ? 'warn' : (b.charging ? 'good' : ''));
    } else if (b && (b.ac || b.noBattery)) {
      setIcon('battery', ICONS.batPlug);
      setVal('battery', 'AC', 'dim');
    } else setVal('battery', '—', 'dim');
  }
  if (segEls.bluetooth) {
    const bt = (stats && stats.bluetooth) || [];
    // Always visible: "—" when nothing reports (Windows never receives battery
    // from many earbuds — vendor-app-only). Tooltip names devices when present.
    if (bt.length) {
      setVal('bluetooth', bt.map((d) => d.level).join('·'), '');
      segEls.bluetooth.root.title = bt.map((d) => `${d.name}: ${d.level}%`).join('\n');
    } else {
      setVal('bluetooth', '—', 'dim');
      segEls.bluetooth.root.title = 'Bluetooth battery (no device reporting right now)';
    }
  }
  if (segEls.clock) {
    setVal('clock', fmtClock(m.clock ? m.clock.format : null, new Date()), 'dim');
  }
}

window.wizbar.onTheme(applyTheme);
window.wizbar.onStats((s) => { stats = s; render(); });
window.wizbar.onTokens((t) => { tokensAgg = t; render(); });

window.wizbar.getTheme().then(applyTheme);
// local clock tick for smooth seconds if the format uses them
setInterval(() => { if (segEls.clock) render(); }, 1000);
