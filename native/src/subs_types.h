// subs_types.h - the subscription-provider records shared by p_subs.c (which
// fetches them) and a front-end translation unit (which paints them).
//
// WHY THIS HEADER EXISTS
// The Windows build compiles p_subs.c inside the amalgamated translation unit,
// so the front-end there sees these types directly. A front-end in its OWN
// translation unit (the Linux bar) needs the same layout to receive the board
// data through cbSubsProvWins. Declaring that struct a second time in the
// front-end would be two definitions free to drift, and a drifted layout does
// not fail to compile - it reads the wrong bytes and paints believable, wrong
// numbers. So p_subs.c itself includes this header: one definition, checked on
// both sides of the seam.
//
// The fields are the ones the fetch already produced; nothing here is derived
// for display. `pct` and `rem` are percentages, `used`/`total` are the raw
// credit counts (-1 / 0 mean "this window reports no numbers"), and `resetAt`
// is a true UTC epoch in ms (0 = unknown). subsChipFormat turns them into text.
#ifndef SUBS_TYPES_H
#define SUBS_TYPES_H

#define SUBS_LABEL_CCH 24 // "Claude/GPT week" fits; a longer group prefix truncates

// One quota window as a provider last reported it. label is the window's own
// name from the vendor ("week", "5h", "day"), already normalised by the
// fetcher, so the renderer never has to interpret a vendor-specific field.
typedef struct {
    wchar_t label[SUBS_LABEL_CCH];
    int pct;      // percent used, 0..100
    int rem;      // percent remaining, 0..100, -1 = pool between resets
    int used;     // credits used, -1 = not reported
    int total;    // credits in the window, 0 = not reported
    long long resetAt; // UTC epoch ms, 0 = unknown
} SubsWin;

#endif // SUBS_TYPES_H
