# Storm test for the NORMAL-window dashboard: minimize-storm and close-storm.
# The dash should be activated (raised, focused) when the covering window goes
# away — never sunk below the terminal.
import ctypes, ctypes.wintypes, subprocess, time

u32 = ctypes.windll.user32
u32.SetForegroundWindow.restype = ctypes.c_bool
u32.GetForegroundWindow.restype = ctypes.c_void_p
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
        if cls(h) == 'Chrome_WidgetWin_1' and rect(h)[2:] == (840, 580) and 'WizBar' in title(h):
            return h
    return None

def find_term():
    for h in zscan():
        if cls(h) == 'CASCADIA_HOSTING_WINDOW_CLASS' and title(h):
            return h
    return None

def find_notepad():
    for h in zscan():
        if cls(h) == 'Notepad' and title(h):
            return h
    return None

def snap(label, dash, term):
    zscan()
    fg = u32.GetForegroundWindow()
    def st(h):
        if h not in order:
            return 'ABSENT'
        return f'z={order.index(h)} vis={int(bool(u32.IsWindowVisible(h)))}'
    print(f'  [{label}] dash {st(dash)} term {st(term)} fgIsDash={fg == dash}')

def summon():
    subprocess.Popen([r'C:\Users\tyanw\work\general\wizbar\node_modules\electron\dist\electron.exe', '.'],
                     cwd=r'C:\Users\tyanw\work\general\wizbar',
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def alt_tap_focus(h):
    u32.keybd_event(0x12, 0, 0, 0)
    u32.keybd_event(0x12, 0, 2, 0)
    u32.SetForegroundWindow(h)

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
    raise SystemExit('no dash')
print('dash:', hex(dash))
snap('after summon', dash, term)

# ---- storm 1: notepad over dash, MINIMIZE it
p = subprocess.Popen(['notepad.exe'])
time.sleep(2.5)
np = find_notepad()
alt_tap_focus(np)
time.sleep(1.0)
snap('notepad fg', dash, term)
u32.PostMessageW(np, 0x0112, 0x0620, 0)  # SC_MINIMIZE
time.sleep(1.5)
snap('AFTER minimize', dash, term)

# ---- storm 2: second notepad over dash, CLOSE it
p2 = subprocess.Popen(['notepad.exe'])
time.sleep(2.5)
np2 = find_notepad()
alt_tap_focus(np2)
time.sleep(1.0)
snap('notepad2 fg', dash, term)
u32.PostMessageW(np2, 0x0010, 0, 0)  # WM_CLOSE
time.sleep(1.5)
snap('AFTER close', dash, term)

ok1 = None
zscan()
ok1 = order.index(dash) < order.index(term) if dash in order and term in order else None
print('RESULT dash-above-term:', ok1)

u32.PostMessageW(dash, 0x0010, 0, 0)
try:
    p.terminate()
except Exception:
    pass
print('done')
