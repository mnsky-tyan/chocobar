import ctypes
u32 = ctypes.windll.user32
ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
found = []
def cb(h, _):
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, c, 256)
    if c.value == 'CASCADIA_HOSTING_WINDOW_CLASS' and u32.IsWindowVisible(h):
        found.append(h)
    return True
u32.EnumWindows(ENUM(cb), None)
for h in found:
    u32.PostMessageW(ctypes.c_void_p(h), 0x0010, 0, 0)  # WM_CLOSE
print('closed', len(found), 'test terminal(s)')
