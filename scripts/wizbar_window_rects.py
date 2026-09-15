import ctypes, ctypes.wintypes
u32 = ctypes.windll.user32
ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
found = []

def cb(h, _):
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, c, 256)
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, t, 256)
    if t.value == 'WizBar' and u32.IsWindowVisible(h):
        r = ctypes.wintypes.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        found.append((hex(h), r.right - r.left, r.bottom - r.top, r.left, r.top))
    return True

u32.EnumWindows(ENUM(cb), None)
print(found)
