import ctypes
import ctypes.wintypes as wt
u32 = ctypes.windll.user32
dwm = ctypes.windll.dwmapi
E = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
DWMWA_CLOAKED = 14
rows = []
def cb(h, _):
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, c, 256)
    if c.value == 'CASCADIA_HOSTING_WINDOW_CLASS':
        p = ctypes.c_uint(); u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        r = wt.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
        cloaked = wt.DWORD(99)
        hr = dwm.DwmGetWindowAttribute(ctypes.c_void_p(h), DWMWA_CLOAKED, ctypes.byref(cloaked), ctypes.sizeof(cloaked))
        rows.append((h, p.value, u32.IsWindowVisible(h), u32.IsIconic(h), cloaked.value if hr == 0 else f'hr={hr}', r.left, r.top, r.right-r.left, r.bottom-r.top))
    return True
u32.EnumWindows(E(cb), None)
print('hwnd pid visible iconic cloaked rect')
for row in rows:
    print(*row)
