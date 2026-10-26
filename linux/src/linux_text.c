// linux_text.c - the shared text helpers, defined once.
//
// See linux_text.h: the wide-to-UTF-8 conversion and the symbol-face lookup were
// written twice each, and the copies had already diverged (one dropped the
// character that did not fit, the other truncated the whole string; one asked
// fontconfig for a family by name, the other by the charset it needs).

#include <fontconfig/fontconfig.h>
#include <stdio.h>

#include "linux_text.h"

void wideToUtf8(const wchar_t *w, char *out, size_t cap) {
    if (!w) { if (cap) out[0] = 0; return; }
    size_t i = 0;
    for (; *w && i + 1 < cap; w++) {
        unsigned cp = (unsigned)*w;
        if (cp < 0x80) out[i++] = (char)cp;
        else if (cp < 0x800 && i + 2 < cap) {
            out[i++] = (char)(0xC0 | (cp >> 6));
            out[i++] = (char)(0x80 | (cp & 0x3F));
        } else if (i + 3 < cap) {
            out[i++] = (char)(0xE0 | (cp >> 12));
            out[i++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[i++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[i] = 0;
}

cairo_font_face_t *symbolFontFace(void) {
    char symFam[128] = "DejaVu Sans";
    FcPattern *pat = FcNameParse((const FcChar8 *)":charset=2715");
    if (pat) {
        FcConfigSubstitute(NULL, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult res = FcResultNoMatch;
        FcPattern *m = FcFontMatch(NULL, pat, &res);
        if (m && res == FcResultMatch) {
            FcChar8 *fam = NULL;
            if (FcPatternGetString(m, FC_FAMILY, 0, &fam) == FcResultMatch && fam)
                snprintf(symFam, sizeof(symFam), "%s", (const char *)fam);
        }
        if (m) FcPatternDestroy(m);
        FcPatternDestroy(pat);
    }
    return cairo_toy_font_face_create(symFam, CAIRO_FONT_SLANT_NORMAL,
                                      CAIRO_FONT_WEIGHT_NORMAL);
}
