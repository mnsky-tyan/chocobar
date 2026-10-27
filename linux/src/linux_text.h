// linux_text.h - the text helpers every Linux front-end shares.
//
// Two problems every window here has, and both were solved more than once
// before this header existed: a wide string from the shared core has to reach
// cairo as UTF-8 (subs.c narrowed labels with a (char) cast while main.c
// converted them properly, so a non-Latin label painted as garbage), and a
// glyph the display family does not carry needs a face that has it (dash.c and
// subs.c each ran their own fontconfig lookup for U+2715). One definition each,
// so a correction lands in every window at once.
#ifndef CHOCOBAR_LINUX_TEXT_H
#define CHOCOBAR_LINUX_TEXT_H

#include <cairo/cairo.h>
#include <stddef.h>
#include <wchar.h>

// Convert a wide string to NUL-terminated UTF-8, which is what cairo_show_text
// takes. A character with no room left in the buffer is dropped and the
// conversion continues; out is always NUL-terminated, and empty when w is NULL.
void wideToUtf8(const wchar_t *w, char *out, size_t cap);

// A font face for glyphs the display family lacks (the titlebar's U+2715 close
// cross). Resolved through fontconfig for a family that actually carries the
// glyph, because cairo's toy API does not fall back and a missing glyph paints
// as a tofu box. The caller owns the face (cairo_font_face_destroy).
cairo_font_face_t *symbolFontFace(void);

#endif
