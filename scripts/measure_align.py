"""Measure bar vs terminal alignment from the latest screenshot PNG.
Scans a horizontal line through the bar and one through the terminal top area,
finding the cream-bar extent and the terminal-frame extent."""
import sys
from PIL import Image

img = Image.open(sys.argv[1]).convert('RGB')
W, H = img.size
px = img.load()
scale = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0

def is_cream(c):
    r, g, b = c
    return r > 215 and g > 200 and b > 180 and r >= g >= b - 10

# scale factor: screenshot W (1280) vs physical 2880
scale = 2880 / W

# 1) bar row: find the widest cream run in the top 40 shot-px
best = None
for y in range(2, 40):
    run_start = None
    for x in range(0, W):
        if is_cream(px[x, y]):
            if run_start is None:
                run_start = x
        else:
            if run_start is not None and (best is None or x - run_start > best[2]):
                best = (run_start, x, x - run_start, y)
            run_start = None
    if run_start is not None and (best is None or W - run_start > best[2]):
        best = (run_start, W, W - run_start, y)

if not best:
    print("no bar found")
    sys.exit(0)
bx0, bx1, bw, by = best
print(f"bar row y={by} shot: x {bx0}..{bx1}  (phys {bx0*scale:.0f}..{bx1*scale:.0f}, w={bw*scale:.0f})")

# 2) terminal top edge: scan down the middle for the first long dark-ish horizontal
# transition (terminal outline). Use column at 3/4 width (avoids sidebar).
col = int(W * 0.72)
prev = None
for y in range(by + 2, 120):
    r, g, b = px[col, y]
    lum = 0.3 * r + 0.6 * g + 0.1 * b
    if prev is not None and prev - lum > 28:
        print(f"terminal top edge at y={y} shot (phys {y*scale:.0f}) under column {col}")
        break
    prev = lum

# 3) terminal frame left/right: scan the terminal's tab row (~20px below its top)
