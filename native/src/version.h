/* Single source of truth for the shipped version. Included by chocobar.c
   (startup log line) and by version.rc (the exe's version resource), and read
   by macos/build.sh for the .app bundle's Info.plist, so none of them can
   drift apart. package.json and package-lock.json repeat the
   number for npm metadata; scripts/portable_regression.js fails the suite
   when the copies disagree. Bump all three, then tag the release. */
#ifndef CB_VERSION_H
#define CB_VERSION_H

#define CB_VER_MAJOR 1
#define CB_VER_MINOR 5
#define CB_VER_PATCH 1
#define CB_VER_STR   "1.5.1"

#endif
