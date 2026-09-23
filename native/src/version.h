/* Single source of truth for the shipped version. Included by chocobar.c
   (startup log line) and by version.rc (the exe's version resource), so the
   two can never drift apart. Bump here, then tag the release. */
#ifndef CB_VERSION_H
#define CB_VERSION_H

#define CB_VER_MAJOR 1
#define CB_VER_MINOR 2
#define CB_VER_PATCH 0
#define CB_VER_STR   "1.2.0"

#endif
