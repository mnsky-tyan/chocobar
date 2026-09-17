'use strict';
// Token dashboard renderer: GitHub-style heatmap + summaries.

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
const WEEKDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
let LEVELS = ['var(--l0)', 'var(--l1)', 'var(--l2)', 'var(--l3)', 'var(--l4)'];

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
  const map = { '--fg': t.fg, '--fg-dim': t.fgDim, '--pink': t.pink, '--pink-deep': t.pinkDeep, '--pink-bg': t.pinkBg, '--yellow': t.yellow, '--warn': t.warn, '--divider': t.divider };
  for (const [k, v] of Object.entries(map)) if (v) r.setProperty(k, v);
  // Heatmap ramp is user-configurable (theme.heatmap, light to dark).
  if (t.heatmap && t.heatmap.length === 5) {
    t.heatmap.forEach((c, i) => r.setProperty(`--l${i}`, c));
  }
  // Card is PINK: follow the theme's pinkBg for the dashboard background and
  // body. (The old translucent card composited over the desktop = murky.)
  if (t.pinkBg) {
    r.setProperty('--bg', t.pinkBg);
    document.body.style.background = t.pinkBg;
  }
  $('btn-subs').classList.toggle('hidden', !(t.subs && t.subs.enabled));
  // Section toggles apply on first open, pushed themes, and hot reload alike.
  applyVisibility();
}

// Harness display name: the config's tokens.labels override wins, else the
// built-in source id. Keeps the product harness-agnostic — nothing in the UI
// is pinned to one vendor's naming.
function appLabel(app) {
  const labels = (theme && theme.labels) || {};
  return labels[app] || app;
}

// Section visibility (tokens.dashboard in the config). Absent key = shown,
// so older payloads and hand-rolled configs keep the full dashboard.
function secOn(k) {
  const d = (theme && theme.tokens && theme.tokens.dashboard) || {};
  return d[k] !== false;
}

function applyVisibility() {
  $('statcards').classList.toggle('hidden', !secOn('stats'));
  $('daily-sec').classList.toggle('hidden', !secOn('heatmap'));
  $('app-sec').classList.toggle('hidden', !secOn('apps'));
  $('model-sec').classList.toggle('hidden', !secOn('models'));
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

  // Empty states are explained instead of bare zeros: usage fully off (the
  // master switch) vs master on with no source configured.
  const usageOff = agg.masterEnabled === false;
  const noSources = !usageOff && (agg.sourcesEnabled || 0) === 0;
  $('no-sources').classList.toggle('hidden', !noSources);
  $('usage-off').classList.toggle('hidden', !usageOff);

  // --- stat cards
  $('statcards').innerHTML =
    statCard('Today', fmt(agg.today.total)) +
    statCard('Last 7 days', fmt(agg.week)) +
    statCard('Last 30 days', fmt(agg.month)) +
    statCard('All time', fmt(agg.allTime));

  renderSubscription();
  renderHeatmap();
  renderTables();
  $('scan-info').textContent = `last scan ${agg.lastScan ? new Date(agg.lastScan).toLocaleTimeString() : '—'}`;
}

function renderSubscription() {
  const sec = $('sub-sec');
  const plans = agg.subscription && agg.subscription.plans;
  if (!plans || !plans.length || !secOn('plans')) { sec.classList.add('hidden'); return; }
  sec.classList.remove('hidden');
  const fmtReset = (iso) => {
    const d = new Date(iso.length === 10 ? iso + 'T12:00:00' : iso);
    return isNaN(d) ? null : `${MONTHS[d.getMonth()]} ${d.getDate()}`;
  };
  $('plans').innerHTML = plans.map((p) => {
    // Clamp the shown percent to the bar; over-spend still reads via used/total.
    const pct = Math.max(0, Math.min(100, Math.round((p.used / p.total) * 100)));
    const reset = p.resetsAt ? fmtReset(p.resetsAt) : null;
    return `<div class="plan-row">` +
      `<div class="plan-top"><span class="plan-name">${esc(p.name)}</span>` +
      `<span class="plan-nums">${fmt(p.used)} / ${fmt(p.total)} · ${pct}%</span></div>` +
      `<div class="plan-bar"><i class="${pct >= 90 ? 'high' : ''}" style="width:${pct}%"></i></div>` +
      (reset ? `<div class="plan-meta">resets ${esc(reset)}</div>` : '') +
      `</div>`;
  }).join('');
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

  // row labels: sparse Sun/Fri; every label row is exactly one cell pitch
  // (11px cell + 3px column gap) so the text sits level with its row — the
  // old Mon/Wed/Fri labels drifted because their line-height didn't match.
  const rowLabels = `<div class="hm-col">` +
    [0, 1, 2, 3, 4, 5, 6].map((dow) =>
      `<span class="hm-row-label">${(dow === 0 || dow === 5) ? WEEKDAYS[dow] : ''}</span>`
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
  applyVisibility();
}

function renderDayDetail() {
  const dd = $('day-detail');
  if (!selectedDay || !secOn('dayDetail')) { dd.classList.add('hidden'); dd.innerHTML = ''; return; }
  const info = (agg.byDay || {})[selectedDay];
  const d = new Date(selectedDay + 'T12:00:00');
  let html = `<div class="d-title">${WEEKDAYS[d.getDay()]}, ${MONTHS[d.getMonth()]} ${d.getDate()}, ${d.getFullYear()} — ${info ? fmt(info.total) + ' tokens' : 'no usage'}</div>`;
  if (info) {
    const aggs = Object.values(info.apps);
    const cache = hasCacheData(aggs);
    html += `<table>`;
    html += '<tr>' + detailHeaders(cache).map((h) => `<td>${h}</td>`).join('') + '</tr>';
    for (const [app, a] of Object.entries(info.apps)) {
      const cells = [`<td class="num">${fmt(a.input)}</td>`, `<td class="num">${fmt(a.output)}</td>`];
      if (cache) cells.push(`<td class="num">${fmt(a.cacheRead)}</td>`, `<td class="num">${fmt(a.cacheWrite)}</td>`);
      cells.push(`<td class="num">${a.requests}</td>`);
      html += `<tr><td><span class="app-dot app-${esc(app)}" title="${esc(app)}"></span>${esc(appLabel(app))}</td>` +
        cells.join('') + '</tr>';
    }
    html += `</table>`;
  }
  dd.innerHTML = html;
  dd.classList.remove('hidden');
}

// Cache columns exist only while some record actually carries cache data; a
// store where nothing ever writes the prompt cache shows no dead columns.
function hasCacheData(aggs) {
  return aggs.some((a) => a && (((a.cacheRead || 0) + (a.cacheWrite || 0)) > 0));
}

const TH_INPUT = '<span title="Prompt tokens. The scanner folds cache reads and writes into this column per source (see the footer note), so input+output is the provider-reported total.">input</span>';
const TH_CACHE_R = '<span title="Prompt-cache READ tokens: cached prefix tokens reported beside the input by the provider.">cache R</span>';
const TH_CACHE_W = '<span title="Prompt-cache WRITE tokens: tokens the provider wrote to the cache on this request (reported as cache_creation / cache.write). Zero when the provider or model does not attribute cache creation.">cache W</span>';

function detailHeaders(cache) {
  return ['app', TH_INPUT, 'output', ...(cache ? [TH_CACHE_R, TH_CACHE_W] : []), 'calls'];
}

function renderTables() {
  // by app
  const apps = Object.entries(agg.byApp || {}).sort((a, b) => totalOfAgg(b[1]) - totalOfAgg(a[1]));
  const appMax = Math.max(1, ...apps.map(([, a]) => totalOfAgg(a)));
  const appCache = hasCacheData(apps.map(([, a]) => a));
  $('app-table').innerHTML = tableHtml(
    ['app', TH_INPUT, 'output', ...(appCache ? [TH_CACHE_R, TH_CACHE_W] : []), 'calls'],
    apps.map(([app, a]) => {
      const cells = [
        `<span class="app-dot app-${esc(app)}" title="${esc(app)}"></span>${esc(appLabel(app))}`,
        fmt(a.input), fmt(a.output)
      ];
      if (appCache) cells.push(fmt(a.cacheRead), fmt(a.cacheWrite));
      cells.push(String(a.requests));
      return cells;
    }),
    apps.map(([, a]) => totalOfAgg(a) / appMax)
  );

  // by model (top 7)
  const models = Object.entries(agg.byModel || {})
    .sort((a, b) => totalOfAgg(b[1]) - totalOfAgg(a[1]))
    .slice(0, 7);
  const modelMax = Math.max(1, ...models.map(([, a]) => totalOfAgg(a)));
  const modelCache = hasCacheData(models.map(([, a]) => a));
  $('model-table').innerHTML = tableHtml(
    ['model', TH_INPUT, 'output', ...(modelCache ? [TH_CACHE_R, TH_CACHE_W] : []), 'calls'],
    models.map(([mk, a]) => {
      const [app, ...rest] = mk.split('|');
      // modelLabel is the tracker's canonical casing for the merged group;
      // fall back to the (lowercased) key for older aggregates.
      const model = a.modelLabel || rest.join('|');
      const cells = [
        `<span class="app-dot app-${esc(app)}" title="${esc(app)}"></span>${esc(model)}`,
        fmt(a.input), fmt(a.output)
      ];
      if (modelCache) cells.push(fmt(a.cacheRead), fmt(a.cacheWrite));
      cells.push(String(a.requests));
      return cells;
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
$('btn-subs').addEventListener('click', () => window.wizbar.openSubs());
$('btn-refresh').addEventListener('click', async () => {
  const btn = $('btn-refresh');
  if (btn.disabled) return;
  btn.disabled = true;
  btn.textContent = '…';
  try {
    const a = await window.wizbar.rescanTokens();
    if (a) { agg = a; render(); }
  } finally {
    btn.disabled = false;
    btn.textContent = 'refresh';
  }
});

document.addEventListener('keydown', (e) => { if (e.key === 'Escape') window.wizbar.close(); });

window.wizbar.onTheme(applyTheme);
window.wizbar.onTokens((a) => { agg = a; render(); });
window.wizbar.getTheme().then(applyTheme);
window.wizbar.getTokens().then((a) => { if (a) { agg = a; render(); } });
