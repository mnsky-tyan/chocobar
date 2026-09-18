'use strict';
// Themed context-menu page. The main process pushes the same themePayload the
// bar gets; the menu surface picks its bg from theme.surfaces.menu (bar tint
// + alpha by default) so the popup matches the bar's color and opacity.
const setVar = (k, v) => { if (v) document.documentElement.style.setProperty(k, v); };

function applyTheme(t) {
  const r = document.documentElement.style;
  const map = {
    '--fg': t.fg, '--fg-dim': t.fgDim, '--pink-deep': t.pinkDeep,
    '--pink-bg': t.pinkBg, '--divider': t.divider
  };
  for (const [k, v] of Object.entries(map)) if (v) r.setProperty(k, v);
  if (t.bar) {
    if (t.bar.radius !== undefined) r.setProperty('--radius', t.bar.radius + 'px');
    if (t.bar.fontFamily) r.setProperty('--font', t.bar.fontFamily);
  }
  const m = t.surfaces && t.surfaces.menu;
  if (m && m.bgCss) r.setProperty('--bg', m.bgCss);
}

window.wizbar.onTheme(applyTheme);

document.addEventListener('keydown', (e) => { if (e.key === 'Escape') window.wizbar.hide(); });

for (const el of document.querySelectorAll('.mi')) {
  // mousedown, not click: the menu should act on press like the bar chips do.
  el.addEventListener('mousedown', (e) => { e.preventDefault(); window.wizbar.action(el.dataset.a); });
}
