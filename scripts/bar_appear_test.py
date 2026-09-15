import ctypes, ctypes.wintypes, subprocess, time
u32 = ctypes.windll.user32
u32.FindWindowW.restype = ctypes.c_void_p
u32.SetWindowPos.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]

_ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

def scan():
    rows = []
    def cb(hwnd, _):
        cls = ctypes.create_unicode_buffer(256); u32.GetClassNameW(hwnd, cls, 256)
        pid = ctypes.c_uint(); u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        r = ctypes.wintypes.RECT(); u32.GetWindowRect(hwnd, ctypes.byref(r))
        if cls.value in ('CASCADIA_HOSTING_WINDOW_CLASS', 'Chrome_WidgetWin_1') and u32.IsWindowVisible(hwnd):
            rows.append((cls.value, pid.value, hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top))
        return True
    u32.EnumWindows(_ENUM(cb), None)
    return rows

subprocess.run(['cmd', '/c', 'start', 'wt.exe'], shell=False)
time.sleep(4)

# move any terminal to mid-screen so there is room above
for cls, pid, hwnd, x, y, w, h in scan():
    if cls == 'CASCADIA_HOSTING_WINDOW_CLASS':
        u32.SetWindowPos(hwnd, None, 140, 400, 0, 0, 0x0001 | 0x0004 | 0x0010)
        print(f"moved terminal pid={pid} -> (140,400)")
time.sleep(2)

print("=== final scan ===")
for cls, pid, hwnd, x, y, w, h in scan():
    print(f"class={cls} pid={pid} pos=({x},{y}) size={w}x{h}")
