import ctypes, time, ctypes.wintypes
u32 = ctypes.windll.user32
u32.SetCursorPos(1300, 60)
time.sleep(0.3)
u32.mouse_event(8, 0, 0, 0, 0)   # RIGHTDOWN
u32.mouse_event(16, 0, 0, 0, 0)  # RIGHTUP
print('right-clicked bar center')
time.sleep(1)
E = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
menus = []
def cb(h, _):
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, c, 256)
    if c.value == '#32768' and u32.IsWindowVisible(h):
        menus.append(h)
    return True
u32.EnumWindows(E(cb), None)
print('context menus visible:', menus)
