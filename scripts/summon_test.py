import ctypes, subprocess, time, ctypes.wintypes
u32 = ctypes.windll.user32

ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

def dash_exists():
    found = []
    def cb(h, _):
        c = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, c, 256)
        r = ctypes.wintypes.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        if c.value == 'Chrome_WidgetWin_1' and u32.IsWindowVisible(h) \
           and (r.right - r.left) == 840 and (r.bottom - r.top) == 580:
            found.append(h)
        return True
    u32.EnumWindows(ENUM(cb), None)
    return found

def click(x, y):
    u32.SetCursorPos(int(x), int(y))
    time.sleep(0.2)
    u32.mouse_event(2, 0, 0, 0, 0)   # LEFTDOWN
    u32.mouse_event(4, 0, 0, 0, 0)   # LEFTUP

def key(vk):
    u32.keybd_event(vk, 0, 0, 0)
def keyup(vk):
    u32.keybd_event(vk, 0, 2, 0)

# restore cursor afterwards
pt = ctypes.wintypes.POINT()
u32.GetCursorPos(ctypes.byref(pt))

# --- test 1: physical click on the token chip (bar far-left, physical coords)
print('test 1: click chip at (184,60)')
click(184, 60)
time.sleep(2)
d = dash_exists()
print('  dashboard open:', bool(d))

# close it again
for h in d:
    u32.PostMessageW(ctypes.c_void_p(h), 0x0010, 0, 0)
time.sleep(1.5)

# --- test 2: Ctrl+Alt+D hotkey
print('test 2: Ctrl+Alt+D')
key(0x11); key(0x12); key(0x44)
time.sleep(0.15)
keyup(0x44); keyup(0x12); keyup(0x11)
time.sleep(2)
d = dash_exists()
print('  dashboard open:', bool(d))

u32.SetCursorPos(pt.x, pt.y)
