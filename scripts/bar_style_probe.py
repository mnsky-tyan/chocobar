import ctypes, ctypes.wintypes
u32 = ctypes.windll.user32
E = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
def cb(h, _):
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, c, 256)
    r = ctypes.wintypes.RECT()
    u32.GetWindowRect(h, ctypes.byref(r))
    if c.value == 'Chrome_WidgetWin_1' and u32.IsWindowVisible(h) and (r.right - r.left) in (1331, 2662) and (r.bottom - r.top) in (36, 72):
        ex = u32.GetWindowLongW(ctypes.c_void_p(h), -20)
        st = u32.GetWindowLongW(ctypes.c_void_p(h), -16)
        print('BAR hwnd', h, 'exstyle=0x%08X style=0x%08X' % (ex & 0xFFFFFFFF, st & 0xFFFFFFFF))
    return True
u32.EnumWindows(E(cb), None)
print('done')
