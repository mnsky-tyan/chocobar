import ctypes
u32 = ctypes.windll.user32
for h in (1773990, 2559854):
    n = u32.GetWindowTextLengthW(ctypes.c_void_p(h))
    buf = ctypes.create_unicode_buffer(n + 1)
    u32.GetWindowTextW(ctypes.c_void_p(h), buf, n + 1)
    ex = u32.GetWindowLongW(ctypes.c_void_p(h), -20)  # GWL_EXSTYLE
    st = u32.GetWindowLongW(ctypes.c_void_p(h), -16)  # GWL_STYLE
    print(h, 'title=%r' % buf.value, 'exstyle=0x%X' % (ex & 0xFFFFFFFF), 'style=0x%X' % (st & 0xFFFFFFFF))
