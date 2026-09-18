'use strict';
// Bar renderer: builds segments right-aligned, updates on pushed stats.

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
const WEEKDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

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
  diamond: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linejoin="round"><path d="M12 3 21 12 12 21 3 12z"/></svg>`,
  // Subscription board chip: a generic gauge. No vendor glyph — the label
  // text and the board content say which provider the number belongs to.
  gauge: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 18a8 8 0 1 1 16 0"/><path d="M12 14l4-4"/></svg>`,
  // Pet bow icon: two loops per side around a small knot.
  bow: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 11C8.5 7.5 5.5 6.5 4.5 8s.5 4 6.5 3"/><path d="M13 11c2.5-3.5 5.5-4.5 6.5-3s-.5 4-6.5 3"/><path d="M11 13c-2.5 3.5-5.5 4.5-6.5 3s.5-4 6.5-3"/><path d="M13 13c2.5 3.5 5.5 4.5 6.5 3s-.5-4-6.5-3"/><circle cx="12" cy="12" r="1.1"/></svg>`,
  // Shortcut chip: a lightning bolt for "runs whatever you wired up".
  bolt: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M13 2 4.5 13.5H11L9.5 22 19 10h-6.5L13 2z"/></svg>`
};

let theme = null;
let stats = null;
let tokensAgg = null;
let petState = null;
let subsAgg = null;
let subsPage = 0;           // which provider the subs chip shows; flips every minute
let segEls = {};
let sizeReportTimer = null;

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
    // Static mode (non-Windows): full-width strip ('workarea', like the bar
    // above a maximized terminal) or corner pill ('content').
    const isStatic = t.bar.mode === 'static-top';
    bar.classList.toggle('static', isStatic);
    bar.classList.toggle('static-content', isStatic && t.bar.staticWidth === 'content');
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
  // Pinned chips (shortcut/pet/tokens/subs) are direct children of #bar —
  // remove stale ones from earlier rebuilds before appending fresh chips.
  for (const n of [...el('bar').querySelectorAll(':scope > #seg-shortcut, :scope > #seg-pet, :scope > #seg-tokens, :scope > #seg-subs')]) n.remove();
  c.innerHTML = '';
  segEls = {};
  if (!theme) return;
  const m = theme.modules || {};
  const pinned = (theme.bar.align || 'right') === 'right';

  // Chip order: shortcut leftmost, then the pet (bow toggle), then the token
  // dashboard, then the subscription board right of it.
  // With the right-aligned group these pin left of #segments as direct
  // children of #bar, and the auto margin that pushes the module group right
  // sits on the LAST pinned chip.
  const chips = [];
  if (m.shortcut && m.shortcut.enabled) {
    const s = seg('shortcut', ICONS.bolt, true);
    s.title = m.shortcut.command ? `Run: ${m.shortcut.command}` : 'Shortcut (set modules.shortcut.command in the config)';
    s.addEventListener('click', () => window.wizbar.runShortcut());
    chips.push(s);
  }
  if (m.pet && m.pet.enabled) {
    const s = seg('pet', ICONS.bow, true);
    s.addEventListener('click', () => window.wizbar.togglePet());
    chips.push(s);
  }
  if (theme.tokens && theme.tokens.showOnBar) {
    const s = seg('tokens', ICONS.diamond, true);
    s.title = 'Token usage today — click to open dashboard';
    s.addEventListener('click', () => window.wizbar.openDash());
    chips.push(s);
  }
  if (theme.subs && theme.subs.enabled) {
    const s = seg('subs', ICONS.gauge, true);
    s.title = 'Subscription plan remaining — click to open board';
    s.addEventListener('click', () => window.wizbar.openSubs());
    chips.push(s);
  }
  if (pinned) {
    chips.forEach((s, i) => {
      // .seg.clickable pulls neighbours 5px into its own hover box with a
      // negative margin - give the buttons real clearance so a hover
      // highlight can only ever cover the chip under the cursor.
      s.style.marginLeft = i === 0 ? '8px' : '4px';
      if (i === chips.length - 1) s.style.marginRight = 'auto'; // group push
      el('bar').insertBefore(s, el('segments'));
    });
  } else {
    for (const s of chips) c.appendChild(s);
  }
  const titles = { gpu: 'GPU usage', cpu: 'CPU usage', cputemp: 'CPU temperature', ram: 'Memory usage', volume: 'Volume', battery: 'Battery', bluetooth: 'Bluetooth device battery', clock: 'Local time' };
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

// Clicking the strip (not a chip) raises the followed terminal - the bar is
// the terminal's title-strip substitute, so activating it activates the
// terminal. mousedown, not click: raise fires on press (feels instant), and
// chips handle their own clicks and stop here via the .seg check.
document.body.addEventListener('mousedown', (e) => {
  if (e.target.closest('.seg, .clickable, button')) return;
  window.wizbar.raiseTerminal();
});

function setVal(id, text, cls) {
  const s = segEls[id];
  if (!s) return;
  // DOM writes are skipped when nothing changed: the 1s clock tick and every
  // stats push re-render every segment, and identical writes still invalidate
  // style/layout for the whole bar (its biggest renderer cost after IPC).
  const className = 'val' + (cls ? ' ' + cls : '');
  if (s.val.textContent === text && s.val.className === className) return;
  s.val.textContent = text;
  s.val.className = className;
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
  return (fmt || '{MMM} {dd} ({Wkk}) {HH}:{mm}')
    .replace('{MMM}', MONTHS[d.getMonth()])
    .replace('{mmmm}', MONTHS[d.getMonth()].toUpperCase())
    .replace('{Wkk}', WEEKDAYS[d.getDay()])
    .replace('{Wkkk}', WEEKDAYS[d.getDay()].toUpperCase())
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

  // Static mode: keep the main process informed of the pill's natural width
  // so the window can shrink to it (corner-pill mode only; see
  // 'bar-content-size' in main.js — full-strip mode ignores this).
  if (theme.bar && theme.bar.mode === 'static-top' &&
      theme.bar.staticWidth === 'content') {
    if (sizeReportTimer == null) {
      sizeReportTimer = setInterval(() => {
        const b = el('bar');
        if (b) window.wizbar.reportSize(b.offsetWidth);
      }, 1000);
    }
  } else if (sizeReportTimer != null) {
    // No longer corner-pill mode: the main process ignores these reports.
    clearInterval(sizeReportTimer);
    sizeReportTimer = null;
  }

  if (segEls.tokens) setVal('tokens', fmtTokens(tokensAgg ? tokensAgg.today.total : null), 'dim');

  if (segEls.shortcut) {
    const sc = (m.shortcut || {});
    setVal('shortcut', sc.label || 'run', 'dim');
  }

  if (segEls.pet) {
    const pl = (m.pet && m.pet.label) || '';
    if (petState && petState.exists) {
      const on = petState.running;
      setVal('pet', pl ? `${pl} ${on ? 'on' : 'off'}` : (on ? 'on' : 'off'), on ? 'good' : 'dim');
      segEls.pet.root.title = on
        ? 'Pet is out — click to send it away'
        : 'Pet — click to summon it';
    } else {
      setVal('pet', pl, 'dim');
      segEls.pet.root.title = 'Pet — exe not found (modules.pet.exePath in config)';
    }
  }

  if (segEls.subs) {
    // Rotate through the enabled providers every minute so every wired plan is
    // visible on the bar without opening the board. Remaining counts down
    // 100% -> 0%; color: >70 green, >30 yellow, else red.
    const providers = (theme.subs && theme.subs.providers) || [];
    const pages = providers.map((p) => {
      const snap = subsAgg && subsAgg.providers && subsAgg.providers[p.id];
      // Week window preferred; else the first window; else no number yet.
      const win = snap && snap.windows
        ? (snap.windows.find((w) => w.key === 'secondary' || /week/i.test(String(w.label))) || snap.windows[0])
        : null;
      return { label: p.label, snap, win };
    });
    const page = pages[subsPage % Math.max(1, pages.length)] || { label: '', snap: null, win: null };
    if (page.snap && page.snap.status === 'stale') {
      setVal('subs', 'stale', 'dim');
    } else if (page.win && page.win.remainingPercent != null) {
      const rem = page.win.remainingPercent;
      setVal('subs', Math.round(rem) + '%', rem > 70 ? 'good' : rem > 30 ? 'dim' : 'warn');
    } else {
      setVal('subs', '—', 'dim');
    }
    segEls.subs.root.title = pages.length
      ? pages.map((pg) => `${pg.label}${pg.snap && pg.snap.plan ? ' ' + pg.snap.plan : ''} week: ${
          pg.win && pg.win.remainingPercent != null ? Math.round(pg.win.remainingPercent) + '% left' : '—'}`
        ).join('\n') + '\nclick for plan board'
      : 'Subscription board — enable subs.providers in the config';
  }

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
      // Plain label on hover regardless of which sensor layer answered
      // (HWiNFO shm, sysfs, ...): the source lives in stats, not in the UI.
      segEls.cputemp.root.title = 'CPU temperature';
    } else {
      setVal('cputemp', '—', 'dim');
      segEls.cputemp.root.title = 'CPU temperature';
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
      // charging. Since the 100%-on-AC fix, native reports charging=true at
      // full charge on AC, so "plugged in at 100%" shows a full body, bolt and
      // the green 'good' class.
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
window.wizbar.onPet((r) => { petState = r; render(); });
window.wizbar.onSubs((s) => { subsAgg = s; render(); });

window.wizbar.getTheme().then(applyTheme);
// local clock tick for smooth seconds if the format uses them; it also flips
// the subs chip to the next provider once a minute
setInterval(() => {
  if (segEls.clock || segEls.subs) {
    if (segEls.subs) subsPage = Math.floor(Date.now() / 60000);
    render();
  }
}, 1000);
