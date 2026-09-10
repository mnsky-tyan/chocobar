import ctypes, sys
u32 = ctypes.windll.user32
_ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
rows = []
def cb(hwnd, _):
    cls = ctypes.create_unicode_buffer(256); u32.GetClassNameW(hwnd, cls, 256)
    pid = ctypes.c_uint(); u32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    r = ctypes.wintypes.RECT(); u32.GetWindowRect(hwnd, ctypes.byref(r))
    vis = u32.IsWindowVisible(hwnd)
    if cls.value in ('CASCADIA_HOSTING_WINDOW_CLASS', 'Chrome_WidgetWin_1') and vis:
        rows.append((cls.value, pid.value, r.left, r.top, r.right - r.left, r.bottom - r.top))
    return True
import ctypes.wintypes
u32.EnumWindows(_ENUM(cb), None)
for c, p, x, y, w, h in rows:
    print(f"class={c} pid={p} pos=({x},{y}) size={w}x{h}")
