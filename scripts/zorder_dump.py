import ctypes, subprocess, re, ctypes.wintypes
u32 = ctypes.windll.user32
E = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
rows = []
def cb(h, _):
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, c, 256)
    p = ctypes.c_uint()
    u32.GetWindowThreadProcessId(h, ctypes.byref(p))
    if u32.IsWindowVisible(h) and c.value in ('Chrome_WidgetWin_1', 'CASCADIA_HOSTING_WINDOW_CLASS'):
        r = ctypes.wintypes.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        rows.append((c.value, h, p.value, r.left, r.top, r.right - r.left, r.bottom - r.top))
    return True
u32.EnumWindows(E(cb), None)

out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq electron.exe', '/FO', 'CSV'],
                     capture_output=True, text=True).stdout
epids = re.findall(r'"electron\.exe","(\d+)"', out)
print('electron pids:', epids)

print('z-order top->bottom (class, hwnd, pid, rect):')
for c, h, p, x, y, w, hh in rows:
    tag = ' <== WIZBAR BAR' if str(p) in epids else ''
    print(f'hwnd={h} pid={p} ({x},{y}) {w}x{hh} {c}{tag}')
