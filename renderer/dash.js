'use strict';
// Token dashboard renderer: GitHub-style heatmap + summaries.

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
const WEEKDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const LEVELS = ['var(--l0)', 'var(--l1)', 'var(--l2)', 'var(--l3)', 'var(--l4)'];

let agg = null;
let theme = null;
let selectedDay = null;

const $ = (id) => document.getElementById(id);

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function fmt(n) {
  if (n == null) return '—';
  if (n >= 1e9) return (n / 1e9).toFixed(2) + 'B';
  if (n >= 1e6) return (n / 1e6).toFixed(1) + 'M';
  if (n >= 1e3) return (n / 1e3).toFixed(1) + 'k';
  return String(Math.round(n));
}

function applyTheme(t) {
  theme = t;
  const r = document.documentElement.style;
  const map = { '--fg': t.fg, '--fg-dim': t.fgDim, '--pink': t.pink, '--pink-deep': t.pinkDeep, '--pink-bg': t.pinkBg, '--yellow': t.yellow, '--divider': t.divider };
  for (const [k, v] of Object.entries(map)) if (v) r.setProperty(k, v);
  // Card is PINK: follow the theme's pinkBg for the dashboard background and
  // body. (The old translucent card composited over the desktop = murky.)
  if (t.pinkBg) {
    r.setProperty('--bg', t.pinkBg);
    document.body.style.background = t.pinkBg;
  }
}

function statCard(label, value, sub) {
  return `<div class="statcard"><div class="label">${esc(label)}</div><div class="value">${value}${sub ? ` <small>${esc(sub)}</small>` : ''}</div></div>`;
}

function totalOfAgg(a) {
  if (!a) return 0;
  // Same convention as the backend: input already includes cached tokens,
  // so totals are input + output only (cache columns are informational).
  return (a.input || 0) + (a.output || 0);
}

function render() {
  if (!agg) return;

  // --- stat cards
  const todayReq = Object.values(agg.today.apps || {}).reduce((n, a) => n + a.requests, 0);
  $('statcards').innerHTML =
    statCard('Today', fmt(agg.today.total), `${todayReq} calls`) +
    statCard('Last 7 days', fmt(agg.week), '') +
    statCard('Last 30 days', fmt(agg.month), '') +
    statCard('All time', fmt(agg.allTime), `${agg.recordCount} records`);

  renderHeatmap();
  renderTables();
  $('scan-info').textContent = `last scan ${agg.lastScan ? new Date(agg.lastScan).toLocaleTimeString() : '—'}`;
}

function dayLevel(total, thresholds) {
  if (!total) return 0;
  if (total <= thresholds[0]) return 1;
  if (total <= thresholds[1]) return 2;
  if (total <= thresholds[2]) return 3;
  return 4;
}

function renderHeatmap() {
  const weeks = agg.heatmapWeeks || 26;
  const byDay = agg.byDay || {};
  // Quantile thresholds over nonzero days: guarantees the shades spread no
  // matter how skewed the usage is (log-scale saturated at ~100M/day usage).
  const totals = Object.values(byDay).map((d) => d.total).filter((t) => t > 0).sort((a, b) => a - b);
  const pick = (p) => (totals.length ? totals[Math.min(totals.length - 1, Math.floor(p * totals.length))] : 1);
  const thresholds = [pick(0.25), pick(0.5), pick(0.75)];

  const today = new Date();
  today.setHours(12, 0, 0, 0);
  const endDow = today.getDay(); // 0=Sun
  const start = new Date(today);
  start.setDate(start.getDate() - ((weeks - 1) * 7 + endDow));

  const cols = [];
  for (let w = 0; w < weeks; w++) {
    const days = [];
    for (let dow = 0; dow < 7; dow++) {
      const d = new Date(start);
      d.setDate(d.getDate() + w * 7 + dow);
      const key = dateKey(d);
      const info = byDay[key];
      const future = d > today;
      days.push({ key, d, total: future ? -1 : (info ? info.total : 0), info });
    }
    cols.push(days);
  }

  let lastMonth = -1;
  const colHtml = cols.map((days) => {
    let mlabel = '';
    const first = days[0].d;
    if (first.getMonth() !== lastMonth && first.getDate() <= 21) {
      lastMonth = first.getMonth();
      mlabel = `<span class="hm-month">${MONTHS[lastMonth]}</span>`;
    }
    const cells = days.map((day) => {
      const lvl = day.total < 0 ? -1 : dayLevel(day.total, thresholds);
      const style = lvl < 0 ? 'visibility:hidden' : `background:${LEVELS[lvl]}`;
      const sel = day.key === selectedDay ? ' sel' : '';
      return `<div class="hm-cell${sel}" style="${style}" data-key="${day.key}"></div>`;
    }).join('');
    return `<div class="hm-col">${mlabel}${cells}</div>`;
  }).join('');

  // row labels: sparse Mon/Wed/Fri
  const rowLabels = `<div class="hm-col"><span class="hm-row-label" style="height:14px"></span>` +
    [0, 1, 2, 3, 4, 5, 6].map((dow) =>
      `<span class="hm-row-label">${(dow === 1 || dow === 3 || dow === 5) ? WEEKDAYS[dow] : ''}</span>`
    ).join('') + `</div>`;

  $('heatmap').innerHTML = rowLabels + colHtml;

  // legend
  $('heat-legend').innerHTML = 'less ' + LEVELS.map((c) => `<i style="background:${c}"></i>`).join('') + ' more';

  // events
  $('heatmap').querySelectorAll('.hm-cell').forEach((cell) => {
    cell.addEventListener('mousemove', (e) => showTip(cell, e));
    cell.addEventListener('mouseleave', hideTip);
    cell.addEventListener('click', () => selectDay(cell.dataset.key));
  });
}

function dateKey(d) {
  const m = String(d.getMonth() + 1).padStart(2, '0');
  const day = String(d.getDate()).padStart(2, '0');
  return `${d.getFullYear()}-${m}-${day}`;
}

function showTip(cell, e) {
  const key = cell.dataset.key;
  const info = agg.byDay[key];
  const tip = $('heat-tip');
  const d = new Date(key + 'T12:00:00');
  let html = `<b>${WEEKDAYS[d.getDay()]}, ${MONTHS[d.getMonth()]} ${d.getDate()}, ${d.getFullYear()}</b><br>`;
  if (info) {
    html += `${fmt(info.total)} tokens<br>`;
    for (const [app, a] of Object.entries(info.apps)) {
      html += `${app}: ${fmt(totalOfAgg(a))} (${a.requests})<br>`;
    }
  } else {
    html += 'no usage';
  }
  tip.innerHTML = html;
  tip.classList.remove('hidden');

  // Anchor to the cursor, prefer above it, flip below when near the screen top,
  // and clamp horizontally — the tooltip never covers the cursor, never leaves
  // the window, and never triggers scrolling.
  const tw = tip.offsetWidth;
  const th = tip.offsetHeight;
  let x = e.clientX + 14;
  if (x + tw > window.innerWidth - 8) x = e.clientX - tw - 14;
  x = Math.max(8, x);
  let y = e.clientY - th - 10;
  if (y < 8) y = e.clientY + 18;
  tip.style.left = x + 'px';
  tip.style.top = y + 'px';
}

function hideTip() { $('heat-tip').classList.add('hidden'); }

function selectDay(key) {
  selectedDay = (selectedDay === key) ? null : key;
  renderHeatmap();
  renderDayDetail();
}

function renderDayDetail() {
  const dd = $('day-detail');
  if (!selectedDay) { dd.classList.add('hidden'); dd.innerHTML = ''; return; }
  const info = (agg.byDay || {})[selectedDay];
  const d = new Date(selectedDay + 'T12:00:00');
  let html = `<div class="d-title">${WEEKDAYS[d.getDay()]}, ${MONTHS[d.getMonth()]} ${d.getDate()}, ${d.getFullYear()} — ${info ? fmt(info.total) + ' tokens' : 'no usage'}</div>`;
  if (info) {
    html += `<table>`;
    html += `<tr><td>app</td><td class="num">input</td><td class="num">output</td><td class="num">cache R</td><td class="num">cache W</td><td class="num">calls</td></tr>`;
    for (const [app, a] of Object.entries(info.apps)) {
      html += `<tr><td><span class="app-dot app-${esc(app)}"></span>${esc(app)}</td><td class="num">${fmt(a.input)}</td><td class="num">${fmt(a.output)}</td><td class="num">${fmt(a.cacheRead)}</td><td class="num">${fmt(a.cacheWrite)}</td><td class="num">${a.requests}</td></tr>`;
    }
    html += `</table>`;
  }
  dd.innerHTML = html;
  dd.classList.remove('hidden');
}

function renderTables() {
  // by app
  const apps = Object.entries(agg.byApp || {}).sort((a, b) => totalOfAgg(b[1]) - totalOfAgg(a[1]));
  const appMax = Math.max(1, ...apps.map(([, a]) => totalOfAgg(a)));
  $('app-table').innerHTML = tableHtml(['app', 'input', 'output', 'cache R', 'cache W', 'calls'],
    apps.map(([app, a]) => [
      `<span class="app-dot app-${esc(app)}"></span>${esc(app)}`,
      fmt(a.input), fmt(a.output), fmt(a.cacheRead), fmt(a.cacheWrite), String(a.requests)
    ]),
    apps.map(([, a]) => totalOfAgg(a) / appMax)
  );

  // by model (top 7)
  const models = Object.entries(agg.byModel || {})
    .sort((a, b) => totalOfAgg(b[1]) - totalOfAgg(a[1]))
    .slice(0, 7);
  const modelMax = Math.max(1, ...models.map(([, a]) => totalOfAgg(a)));
  $('model-table').innerHTML = tableHtml(['model', 'input', 'output', 'cache R', 'cache W', 'calls'],
    models.map(([mk, a]) => {
      const [app, ...rest] = mk.split('|');
      const model = rest.join('|');
      return [`<span class="app-dot app-${esc(app)}" title="${esc(app)}"></span>${esc(model)}`,
        fmt(a.input), fmt(a.output), fmt(a.cacheRead), fmt(a.cacheWrite), String(a.requests)];
    }),
    models.map(([, a]) => totalOfAgg(a) / modelMax)
  );
}

function tableHtml(headers, rows, shares) {
  let h = '<table class="dt"><tr>' + headers.map((x) => `<th>${x}</th>`).join('') + '</tr>';
  rows.forEach((row, i) => {
    const share = shares[i] || 0;
    h += '<tr>' + row.map((cell, ci) => {
      if (ci === 0) return `<td class="share"><i style="width:${Math.round(share * 100)}%"></i><span>${cell}</span></td>`;
      return `<td>${cell}</td>`;
    }).join('') + '</tr>';
  });
  return h + '</table>';
}

$('btn-close').addEventListener('click', () => window.wizbar.close());
$('btn-refresh').addEventListener('click', () => { window.wizbar.getTokens().then((a) => { if (a) { agg = a; render(); } }); });

document.addEventListener('keydown', (e) => { if (e.key === 'Escape') window.wizbar.close(); });

window.wizbar.onTheme(applyTheme);
window.wizbar.onTokens((a) => { agg = a; render(); });
window.wizbar.getTheme().then(applyTheme);
window.wizbar.getTokens().then((a) => { if (a) { agg = a; render(); } });

// Live system readout (CPU temperature via HWiNFO when available); hidden entirely
// while no sensor reports so the dashboard stays token-only in shape.
function renderSysInfo(s) {
  const el = $('sys-info');
  if (!el) return;
  const t = s && s.cpuTemp;
  const txt = t && t.state === 'ok' && t.c != null ? ` · CPU ${t.c % 1 ? t.c.toFixed(1) : t.c.toFixed(0)}°C` : '';
  el.textContent = txt;
  el.classList.toggle('hidden', !txt);
}
window.wizbar.onStats(renderSysInfo);
window.wizbar.getStats().then(renderSysInfo);
