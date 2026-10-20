// selfheal.h - the bar's one-shot KWin integration fixups at startup.
//
// The bar is not draggable and takes no window-manager chrome, but the DESKTOP
// still decorates it behind its back: the KDE-Rounded-Corners effect
// (kwin4_effect_shapecorners) draws a 1px black outline around every inactive
// window it manages, ON TOP of the client's own pixels. The bar paints its own
// hairline border in the theme divider colour, so the effect's outline reads as
// a black line sitting on the border - the "broken border" defect. Measured on
// this desktop (2026-10-20): the bar's row 0 composited as (0,0,0) while the
// window's own pixel was alpha 154 border; unloading the effect restored
// (239,229,216). Excluding the bar by window class fixes it without touching
// the effect for any other window.
//
// The exclusion lives in ~/.config/kwinrc under [Round-Corners] Exclusions
// (a KConfig string list, comma separated; the effect matches each entry as a
// case-insensitive substring of the window class, and the bar's class is
// "chocobar Chocobar"). This module appends "chocobar" when missing and asks
// KWin to re-read its config, so a fresh install needs no manual step - the
// same spirit as --install-geometry for the geometry helper.
#ifndef SELFHEAL_H
#define SELFHEAL_H

// Returns 1 if kwinrc was edited (and KWin was notified), 0 when the exclusion
// was already in place or nothing could be done (no kwinrc path, no session
// bus - both are non-fatal: the bar still runs, it just keeps the outline).
int cbKwinSelfHeal(void);

#endif
