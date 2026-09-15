import ctypes, ctypes.wintypes
u32 = ctypes.windll.user32

class POINT(ctypes.Structure):
    _fields_ = [('x', ctypes.c_long), ('y', ctypes.c_long)]

for (x, y) in [(184, 60), (700, 60), (1300, 60)]:
    pt = POINT(x, y)
    h = u32.WindowFromPoint(pt)
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(ctypes.c_void_p(h), c, 256)
    p = ctypes.c_uint()
    u32.GetWindowThreadProcessId(ctypes.c_void_p(h), ctypes.byref(p))
    ex = u32.GetWindowLongW(ctypes.c_void_p(h), -20)
    print(f"point ({x},{y}) -> hwnd={h} class={c.value} pid={p.value} exstyle=0x{ex & 0xFFFFFFFF:08X}")
