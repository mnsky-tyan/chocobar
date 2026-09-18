'use strict';
// Subscription Board renderer: donut pies per plan window (5h / week).
// Numbers count down as REMAINING available (100% -> 0%), matching the bar chip.
// Panels are built from the tracker snapshot, so whatever providers the user
// wired in subs.providers render here without renderer changes.

let state = null;

const $ = (id) => document.getElementById(id);

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function fmtReset(ms) {
  if (!ms) return 'reset —';
  const delta = ms - Date.now();
  if (delta <= 0) return 'reset now';
  const h = Math.floor(delta / 3600000);
  const m = Math.floor((delta % 3600000) / 60000);
  const d = Math.floor(h / 24);
  if (d >= 1) return `reset in ${d}d ${h % 24}h`;
  if (h >= 1) return `reset in ${h}h ${m}m`;
  return `reset in ${m}m`;
}

function statusLabel(s) {
  return ({
    ok: 'ok',
    stale: 'stale',
    critical: 'near cap',
    blocked: 'capped',
    'no-auth': 'no login',
    'auth-expired': 'auth stale',
    error: 'error',
    unknown: ''
  })[s] ?? (s || '');
}

function applyTheme(t) {
  if (!t) return;
  const r = document.documentElement.style;
  const map = {
    '--fg': t.fg, '--fg-dim': t.fgDim, '--pink': t.pink, '--pink-deep': t.pinkDeep,
    '--pink-bg': t.pinkBg, '--yellow': t.yellow, '--warn': t.warn, '--good': t.good,
    '--divider': t.divider
  };
  for (const [k, v] of Object.entries(map)) if (v) r.setProperty(k, v);
  // Same as the token dash: board surface follows the theme's pinkBg.
  if (t.pinkBg) {
    r.setProperty('--bg', t.pinkBg);
    document.body.style.background = t.pinkBg;
  }
}

// Donut: remaining arc (countdown) over a pale used track. Always pale pink -
// status still reads from the number and the status pill.
function pieSvg(remaining) {
  const rem = Math.min(100, Math.max(0, remaining));
  const dash = rem.toFixed(1);
  return `<svg class="pie" viewBox="0 0 42 42" aria-hidden="true">
    <circle class="pie-track" cx="21" cy="21" r="15.9155" />
    <circle class="pie-arc" cx="21" cy="21" r="15.9155"
      stroke="var(--pie-pink)"
      stroke-dasharray="${dash} ${(100 - rem).toFixed(1)}"
      stroke-dashoffset="25" />
  </svg>`;
}

function fmtNum(n) {
  if (n == null) return '—';
  if (n >= 1e6) return (n / 1e6).toFixed(1) + 'M';
  if (n >= 1e3) return (n / 1e3).toFixed(1) + 'k';
  return String(Math.round(n));
}

function panelHtml(key, p, i) {
  // Panel dot color cycles through the theme ramp so any number of user-wired
  // providers reads distinctly without hard-coding vendor colors.
  const ramp = (window.__hmRamp && window.__hmRamp.length) ? window.__hmRamp : ['var(--pink-deep)'];
  const dot = ramp[i % ramp.length];
  return `<section class="panel" id="panel-${esc(key)}">
    <header class="panel-head">
      <div class="panel-id">
        <span class="dot" style="background:${dot}"></span>
        <span class="panel-name">${esc(p.label || key)}</span>
        <span class="panel-plan" data-plan>—</span>
      </div>
      <span class="pill" data-pill>—</span>
    </header>
    <div class="panel-body">
      <div class="pies" data-pies></div>
      <div class="panel-foot">
        <span data-detail></span>
      </div>
    </div>
  </section>`;
}

function renderProvider(key, p, idx) {
  // getElementById is an EXACT attribute match: the id is emitted verbatim as
  // 'panel-' + key (provider ids contain colons), so no CSS escaping here -
  // CSS.escape would produce a different string that never matches.
  const panel = document.getElementById('panel-' + key);
  if (!panel || !p) return;
  panel.querySelector('[data-plan]').textContent = p.plan ? p.plan : '—';
  const pill = panel.querySelector('[data-pill]');
  const label = statusLabel(p.status);
  pill.textContent = label;
  pill.className = 'pill ' + (p.status || 'unknown');
  // No label (e.g. plain "warn") → hide the capsule instead of an empty blob.
  pill.classList.toggle('hidden', !label);

  const pies = panel.querySelector('[data-pies]');
  if (!p.windows || !p.windows.length) {
    pies.innerHTML = `<div class="pie-empty">${p.errors && p.errors.length ? esc(p.errors[0]) : 'no windows reported'}</div>`;
  } else {
    pies.innerHTML = p.windows.map((w) => {
      const rem = w.remainingPercent != null
        ? w.remainingPercent
        : (100 - (w.percent || 0));
      // Same integer rounding as the bar chip - the two views render the
      // same snapshot and must never appear to disagree by a rounding step.
      const shown = String(Math.round(rem));
      const usedLine = w.used != null
        ? `${fmtNum(w.used)} / ${fmtNum(w.total)} used`
        : (w.percent != null ? `${Math.round(w.percent)}% used` : '');
      return `<div class="pie-card">
        <div class="pie-wrap">
          ${pieSvg(rem)}
          <div class="pie-center">
            <span class="pie-pct">${shown}<small>%</small></span>
            <span class="pie-cap">left</span>
          </div>
        </div>
        <div class="pie-meta">
          <div class="pie-label">${esc(w.label)}</div>
          <div class="pie-sub">${esc(usedLine)}</div>
          <div class="pie-sub">${esc(fmtReset(w.resetAt))}</div>
        </div>
      </div>`;
    }).join('');
  }

  panel.querySelector('[data-detail]').textContent =
    p.fetchedAt ? new Date(p.fetchedAt).toLocaleTimeString() : '';
}

function render() {
  if (!state) return;
  const panels = $('panels');
  // Disabled providers (switched off in the config) are not rendered at all —
  // a panel with no windows would only take space. No notes/warnings box:
  // a stale fetch shows the 'stale' pill on the panel instead.
  const entries = Object.entries(state.providers || {})
    .filter(([, p]) => p && p.status !== 'disabled');
  // Rebuild panel shells only when the provider set changes; otherwise update
  // in place (keeps the refresh button from flickering the layout).
  const sig = entries.map(([k, p]) => k + ':' + p.label).join('|');
  if (panels.dataset.sig !== sig) {
    panels.dataset.sig = sig;
    panels.innerHTML = entries.map(([k, p], i) => panelHtml(k, p || {}, i)).join('');
  }
  entries.forEach(([k, p], i) => renderProvider(k, p || {}, i));
  $('scan-info').textContent = `last scan ${state.lastScan ? new Date(state.lastScan).toLocaleTimeString() : '—'}`;
}

$('btn-close').addEventListener('click', () => window.wizbar.close());
$('btn-refresh').addEventListener('click', async () => {
  const btn = $('btn-refresh');
  if (btn.disabled) return;
  btn.disabled = true;
  btn.textContent = '…';
  try {
    const s = await window.wizbar.rescanSubs();
    if (s) { state = s; render(); }
  } finally {
    btn.disabled = false;
    btn.textContent = 'refresh';
  }
});

document.addEventListener('keydown', (e) => { if (e.key === 'Escape') window.wizbar.close(); });

window.wizbar.onTheme((t) => {
  if (t && Array.isArray(t.heatmap)) window.__hmRamp = t.heatmap;
  applyTheme(t);
});
window.wizbar.onSubs((s) => { state = s; render(); });
window.wizbar.getTheme().then((t) => {
  if (t && Array.isArray(t.heatmap)) window.__hmRamp = t.heatmap;
  applyTheme(t);
});
window.wizbar.getSubs().then((s) => { if (s) { state = s; render(); } });
setInterval(() => { if (state) render(); }, 30000);
