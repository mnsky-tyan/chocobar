import ctypes, ctypes.wintypes
u32 = ctypes.windll.user32
ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

def cb(h, _):
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, c, 256)
    if c.value == 'Chrome_WidgetWin_1' and u32.IsWindowVisible(h):
        t = ctypes.create_unicode_buffer(256)
        u32.GetWindowTextW(h, t, 256)
        r = ctypes.wintypes.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        print(f'{hex(h)}  {r.right-r.left}x{r.bottom-r.top} @({r.left},{r.top})  {t.value[:60]!r}')
    return True
u32.EnumWindows(ENUM(cb), None)
