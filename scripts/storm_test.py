import os
# WizBar checkout to launch (set WIZBAR_HOME if not the default location)
WIZBAR = os.environ.get('WIZBAR_HOME', r'C:\path\to\wizbar')
ELECTRON = os.path.join(WIZBAR, 'node_modules', 'electron', 'dist', 'electron.exe')
# Full storm experiment: summon dash, snapshot state, notepad storm (real
# SC_MINIMIZE), snapshot at intervals. State = iconic/visible/z-index.
import ctypes, ctypes.wintypes, subprocess, time

u32 = ctypes.windll.user32
u32.SetForegroundWindow.restype = ctypes.c_bool
ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

order = []

def collect(h, _):
    order.append(h)
    return True

CB = ENUM(collect)

def zscan():
    order.clear()
    u32.EnumWindows(CB, None)
    return order

def cls(h):
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, c, 256)
    return c.value

def title(h):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, t, 256)
    return t.value

def rect(h):
    r = ctypes.wintypes.RECT()
    u32.GetWindowRect(h, ctypes.byref(r))
    return (r.left, r.top, r.right - r.left, r.bottom - r.top)

def find_dash():
    for h in zscan():
        if cls(h) == 'Chrome_WidgetWin_1' and title(h) == 'WizBar' and rect(h)[2:] == (840, 580):
            return h
    return None

def find_term():
    for h in zscan():
        if cls(h) == 'CASCADIA_HOSTING_WINDOW_CLASS' and title(h):
            return h
    return None

def snap(label, dash, term):
    zscan()
    def st(h):
        if h not in order:
            return 'ABSENT'
        return f'z={order.index(h)} vis={int(bool(u32.IsWindowVisible(h)))} iconic={int(bool(u32.IsIconic(h)))} rect={rect(h)}'
    print(f'  [{label}] dash {st(dash)} | term {st(term)}')

def summon():
    subprocess.Popen([ELECTRON, '.'],
                     cwd=WIZBAR,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

term = find_term()
print('term:', hex(term))
summon()
dash = None
for _ in range(40):
    time.sleep(0.25)
    dash = find_dash()
    if dash:
        break
if not dash:
    print('NO DASH AFTER SUMMON')
    raise SystemExit
print('dash:', hex(dash))
snap('after summon', dash, term)

# notepad storm
p = subprocess.Popen(['notepad.exe'])
time.sleep(2.5)
np = None
for h in zscan():
    if cls(h) == 'Notepad' and title(h):
        np = h
    if np is None and cls(h) == 'Chrome_WidgetWin_1' and 'Notepad' in title(h):
        np = h
print('notepad:', hex(np) if np else None)
u32.keybd_event(0x12, 0, 0, 0)
u32.keybd_event(0x12, 0, 2, 0)
u32.SetForegroundWindow(np)
time.sleep(1.2)
snap('notepad foreground', dash, term)

t0 = time.time()
u32.PostMessageW(np, 0x0112, 0x0620, 0)  # WM_SYSCOMMAND SC_MINIMIZE
for ms in (0.3, 1.0, 2.0, 3.5):
    while time.time() - t0 < ms:
        time.sleep(0.05)
    snap(f'T+{ms:.1f}s', dash, term)

u32.PostMessageW(dash, 0x0010, 0, 0)
try:
    p.terminate()
except Exception:
    pass
print('done — now check debug.log dash events')
