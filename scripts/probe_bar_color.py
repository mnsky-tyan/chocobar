import ctypes, ctypes.wintypes
u32 = ctypes.windll.user32
ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

wins = []
def cb(h, _):
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, c, 256)
    r = ctypes.wintypes.RECT()
    u32.GetWindowRect(h, ctypes.byref(r))
    wins.append((c.value, h, r.left, r.top, r.right - r.left, r.bottom - r.top, u32.IsWindowVisible(h)))
    return True
u32.EnumWindows(ENUM(cb), None)

# bar = visible Electron window, slim & wide
bars = [w for w in wins if w[0] == 'Chrome_WidgetWin_1' and w[6] and 30 <= w[5] <= 50 and 500 <= w[4] <= 3000]
if not bars:
    print('BAR NOT FOUND')
    raise SystemExit(1)
cls, hwnd, x, y, w, h, _ = bars[0]
print(f'bar at ({x},{y}) {w}x{h}')

gdi32 = ctypes.windll.gdi32
hdc = u32.GetDC(None)
samples = []
for i in range(1, 10):
    px = x + int(w * i / 10)
    col = gdi32.GetPixel(hdc, px, y + h // 2)
    samples.append((col & 255, (col >> 8) & 255, (col >> 16) & 255))
u32.ReleaseDC(None, hdc)
print('samples RGB:', samples)
red = sum(1 for r, g, b in samples if r > g + 60 and r > b + 60)
cream = sum(1 for r, g, b in samples if r > 235 and g > 225 and b > 195)
print(f'red-dominant: {red}/9  cream: {cream}/9')
print('VERDICT:', 'RED' if red >= 5 else ('CREAM' if cream >= 5 else 'CLEAR/OTHER'))
