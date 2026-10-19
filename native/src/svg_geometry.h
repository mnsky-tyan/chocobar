// svg_geometry.h - the shared icon geometry: the path data's shape and the
// portable accessors onto it.
//
// p_svg.c OWNS this geometry (it flattens the SVG paths into polylines). The
// Windows renderer in p_icons.c and the Linux renderer in linux/src/icons_cairo.c
// both consume it, so the type definitions live here and each renderer includes
// this header instead of declaring its own copy. A renderer that disagreed with
// the flattener about the shape of SvgPath would silently misdraw every icon.
//
// Under CHOCOBAR_PORTABLE the two functions below are the seam a front-end in its
// own translation unit uses: the Windows build compiles p_svg.c inside the
// amalgamated translation unit and calls svgFlattenAll()/g_flat directly (as it
// always has), so these accessors are compiled only for the portable builds and
// the Windows binary gains no symbols.
#ifndef SVG_GEOMETRY_H
#define SVG_GEOMETRY_H

// icon ids (order = the path table in p_svg.c)
enum {
    SVG_BOW, SVG_DIAMOND, SVG_GAUGE, SVG_BOLT, SVG_CLOCK,
    SVG_GPU, SVG_CPU, SVG_TEMP, SVG_RAM, SVG_VOL, SVG_MUTE,
    SVG_BAT, SVG_COUNT
};

#define SVG_MAXSUB 12
#define SVG_MAXPTS 512
#define SVG_MAXPARTS 3

typedef struct { float x, y; } SvgPt;
typedef struct { SvgPt p[SVG_MAXPTS]; int n; int closed; } SvgSub;
typedef struct { SvgSub s[SVG_MAXSUB]; int nsub; } SvgPath;
typedef struct { SvgPath path; float w; } SvgFlat;

#ifdef CHOCOBAR_PORTABLE
// Flatten every built-in path once, at startup.
void cbSvgFlattenAll(void);
// The flattened parts of one icon id, and how many there are. NULL for an
// unknown id. The pointer stays valid for the process lifetime.
const SvgFlat *cbSvgFlatById(int id, int *nparts);
#endif

#endif
