import ctypes
u32 = ctypes.windll.user32
u32.FindWindowW.restype = ctypes.c_void_p
h = u32.FindWindowW('CASCADIA_HOSTING_WINDOW_CLASS', None)
print('terminal hwnd:', h)
if h:
    u32.SetForegroundWindow(ctypes.c_void_p(h))
    print('terminal activated')
