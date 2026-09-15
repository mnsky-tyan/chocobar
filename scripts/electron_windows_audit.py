# Enumerate all windows belonging to WizBar's electron processes; an Electron
# Tray icon would appear as a small tooltip-titled top-level window ("WizBar").
import ctypes, ctypes.wintypes

u32 = ctypes.windll.user32
ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

rows = []

def cb(h, _):
    pid = ctypes.wintypes.DWORD()
    u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, c, 256)
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, t, 256)
    rows.append((pid.value, c.value, t.value, bool(u32.IsWindowVisible(h))))
    return True

u32.EnumWindows(ENUM(cb), None)

# electron pids = processes named electron.exe
import subprocess
out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq electron.exe', '/FO', 'CSV', '/NH'],
                     capture_output=True, text=True).stdout
pids = {int(l.split('","')[1]) for l in out.splitlines() if l.startswith('"')}

print('electron pids:', pids)
wiz = [r for r in rows if r[0] in pids]
for r in wiz:
    print(f'  pid={r[0]} class={r[1]!r} title={r[2]!r} visible={r[3]}')

tray_like = [r for r in wiz if r[2] == 'WizBar' or 'tray' in r[1].lower()]
print('TRAY-LIKE WINDOWS:', tray_like if tray_like else 'none')
