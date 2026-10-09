# Codebase Review - 2026-10-10 (round 5)
Scope: /home/tyan/mnsky/chocobar | Review units: native/src (C sources)
Base: f8dafaf (PR #75 merged)
Summary: **No findings.**

This round was a convergence check. Rounds 1-4 found 26, 11, 5 and 3 findings
respectively, and round 4's four commits (`b099f18`, `8d2a8d7`, `d8a98d7`,
`921869a`) all touched the same small area of `p_tokens.c`/`p_utils.c`, so this
pass audited those changes and their edges specifically, then swept the rest of
the tree for the classes of defect the earlier rounds kept surfacing.

## What was verified

**Round 4's changes and their edges.**

- `tokJsonEsc`/`tokJsonUnesc` (`p_tokens.c:577-599`): the writer's `n + need > cap - 1`
  test leaves room for the terminator at every step, and the decoder only ever
  shrinks in place. The save-site buffers are sized from the struct caps:
  `escP[1040]` against `c->path[520]` (max 519 chars, worst case 1038 escaped),
  `escR[160]` against `c->real[80]` (max 79, worst case 158). Both hold.
- The cursor round-trip was **proved with a compiled probe** using the exact
  format string, the vendored `jsmn`, and a real
  `\\wsl.localhost\Ubuntu\home\tyan\...` path: the document parses
  (`jsmn_parse` count pass = 11), `tokJsonUnesc` returns the identical string
  (`strcmp == 0`), and `node -e "JSON.parse(...)"` accepts the same file. Before
  the fix `node` rejected it with `Bad escaped character in JSON at position 21`.
- The load side's `pathA[520]` with `pl < 519` and `c->real[80]` with
  `jstrCopyA`'s `sizeof(c->real)` bound: the in-place decode cannot exceed the
  input length, so `pathA[pl] = 0` and `c->real[len] = 0` stay in range.
- The `TOK_CHARS_NONE` sentinel across all three writers: `tokCursorGet` seeds a
  new entry to `-1`; the truncation branch (`p_tokens.c:447`, fixed by `d8a98d7`)
  writes `-1` because a shrunken file has no known count; `tokScanFile` writes a
  genuine `0` for a flat-format store. Line 494's `cur->chars > 0` carry and line
  758's `c->chars < 0` re-read gate are consistent with all three.
- `stripLineComments`' new escape handling (`p_utils.c:49-58`) was **proved with a
  compiled probe** over seven cases, including the decisive one: a cursor line
  whose path contains an escaped quote keeps string parity, so a `//` inside a
  later string survives while a real trailing `//` comment is still stripped. It
  also fixes a pre-existing latent bug on the config-file path it shares, where a
  value containing `\"` used to flip parity and blank the rest of the line.
- `dibSlot`/`iconDrop` (round 4's icon cache fix): `dibSlot` rejects
  out-of-range ids and the array is `g_iconDib[SVG_ID_MAX]` with
  `SVG_COUNT + SVG_USER_MAX == SVG_ID_MAX`, so `SVG_COUNT + (id - SVG_USER_BASE)`
  cannot exceed the last index.

**Sweep for the classes the earlier rounds kept finding.**

- Every `sprintf`/`lstrcpy` into a fixed buffer is bounded: `expN[32]` holds any
  `long long`; `tk->k[i][32]` receives at most 23 chars from `kIn[24]` via
  `tokFieldCopy` (27 bytes worst case); `p_subs.c:1627`'s `line[400]` holds
  `lab[64]` + `plan[24]` + three ints + format overhead (339 bytes worst case);
  `p_ui.c`'s crash and ULW buffers are ~37 and ~18 bytes into 80 and 64.
- Every computed-length `memcpy` is bounded: `tokPendPush` clamps `alen` to 19 and
  `mlen` to 40 against `app[20]`/`model[41]`; `pv[40]` clamps `pl` to 39; the
  `real` provider list's `realSeen + need < sizeof(real) - 1` guard covers the
  comma, the copy and the terminator; `r.prov` points into the line buffer with
  `provLen` bytes valid, so the clamped read cannot over-read.
- The SM2 reader (`p_metrics.c:225-250`) still holds: `rec + 268 + 8` and
  `rec + 284 + 8` are inside `readingSize >= 320`, the sensor-name read is gated
  by the clamped `sensorCount` with `id < sensorCount`, and `label[64]`/
  `sensor[64]` are written at index at most 63.
- The antigravity auth splice (`p_subs.c:674-736`) is safe: `out` is
  `len + 13874` and the worst-case output is `len + 13850` (three new values at
  their full caps plus six quote bytes, with old values allowed to be empty),
  leaving 24 bytes of margin before `out[o] = 0`.
- Config-controlled array bounds: `tokSrcCount < MAX_TOK_SRC` and
  `subsProviderCount < MAX_SUBS` guard every parse loop, the increment sits
  inside the guard, and the UI clamps `pn` to `MAX_SUBS` before iterating.
- Thread ownership: `g_tokCursor` is touched only inside `p_tokens.c`, and
  `tokLiveInit` runs once from the window-setup path (`p_ui.c:4072`) before
  `loadConfig()` and before the scan thread is created (`p_ui.c:890`), so the
  reset in `tokCursorLoad` cannot race the worker.
- `subsPathExpand` is never handed NULL: four call sites default a NULL raw
  value, two guard with `*x`, and the token scan guards `!s->sessionsDir`. Its
  `ul < outCch` check keeps the appended remainder inside the caller's buffer.
- The estimator's accumulation is correct: `chars` carries the prior total, each
  line's estimate uses `chars + lineChars` (this line is part of what the next
  turn sends), and `chars += lineChars` follows the line's processing.
- No integer overflow in the icon renderer: `bw = 12 * g_scale` with
  `g_scale = LOGPIXELSX / 96.0`, so `w * h * 4` stays far inside `int` at any DPI
  Windows can report.
- `tokJsonStr`'s `\uXXXX` handling: `p + 5 < e` is exactly the bound for six
  bytes, and a truncated escape falls through to the two-byte case without
  over-reading.

## Gate state

`bash native/build.sh --check` exits 0 and `npm test` reports all portable
checks passing on `f8dafaf`. Your live release bar is untouched: PID 4516,
`chocobar.exe` md5 `e703ed840da46001db0a17390f0b4d9e`.

## Conclusion

Nothing meets the reporting bar. Round 4's four interacting fixes are correct
and their edges are closed, including the two follow-on edges the pipeline's own
review found and fixed during that round (`d8a98d7`, `921869a`). I would stop the
loop here rather than manufacture findings: the counts 26 -> 11 -> 5 -> 3 -> 0 are
the shape of a series that has finished.
