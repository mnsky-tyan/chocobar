# Faithful repro of the user's dashboard-sink flow with a REAL minimize
# (WM_SYSCOMMAND SC_MINIMIZE = what the minimize button sends), plus a
# fine-grained z-order timeline so we can watch the sink and the guard's
# response live.
import ctypes, subprocess, time, ctypes.wintypes, sys

u32 = ctypes.windll.user32
u32.FindWindowW.restype = ctypes.c_void_p
u32.GetForegroundWindow.restype = ctypes.c_void_p
ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

def zpos(target):
    # returns z index of target hwnd in EnumWindows order (0 = top), -1 if absent
    found = [-1]
    box = [0]
    def cb(h, _):
        if h == target:
            found[0] = box[0]
            return False  # stop enumeration
        box[0] += 1
        return True
    ENUMCB = ENUM(cb)
    u32.EnumWindows(ENUMCB, None)
    return found[0]

def find_by_class(cls, titled=True):
    out = []
    def cb(h, _):
        c = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, c, 256)
        if c.value == cls and u32.IsWindowVisible(h) and not u32.IsIconic(h):
            if not titled or u32.GetWindowTextLengthW(h) > 0:
                out.append(h)
        return True
    u32.EnumWindows(ENUM(cb), None)
    return out

def titles(h):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, t, 256)
    return t.value

# pick the middle app: a big visible normal window that is neither the terminal
# nor the 840x580 dash nor the bar
def find_middle_app():
    cands = []
    def cb(h, _):
        c = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, c, 256)
        if c.value == 'Chrome_WidgetWin_1' and u32.IsWindowVisible(h) and not u32.IsIconic(h) \
                and u32.GetWindowTextLengthW(h) > 0:
            r = ctypes.wintypes.RECT()
            u32.GetWindowRect(h, ctypes.byref(r))
            w, hh = r.right - r.left, r.bottom - r.top
            if (w, hh) != (840, 580) and w > 400 and hh > 300:
                cands.append((h, w, hh, titles(h)))
        return True
    u32.EnumWindows(ENUM(cb), None)
    return cands

def summon_dash():
    subprocess.run([r'C:\Users\tyanw\work\general\wizbar\node_modules\electron\dist\electron.exe', '.'],
                   cwd=r'C:\Users\tyanw\work\general\wizbar', capture_output=True, timeout=15)

def alt_tap_focus(h):
    u32.keybd_event(0x12, 0, 0, 0)
    u32.keybd_event(0x12, 0, 2, 0)
    u32.SetForegroundWindow(h)

term = find_by_class('CASCADIA_HOSTING_WINDOW_CLASS')[0]
print('terminal:', hex(term), repr(titles(term)[:40]))

summon_dash()
time.sleep(3)
dash = find_by_class('Chrome_WidgetWin_1', titled=False)
dash = [h for h in dash if (lambda r: (r.right - r.left, r.bottom - r.top) == (840, 580))(
    (lambda r: r)(ctypes.wintypes.RECT()))]
# simpler: refilter by rect
def rect(h):
    r = ctypes.wintypes.RECT()
    u32.GetWindowRect(h, ctypes.byref(r))
    return r
dash = [h for h in find_by_class('Chrome_WidgetWin_1', titled=False)
        if (rect(h).right - rect(h).left, rect(h).bottom - rect(h).top) == (840, 580)]
dash = dash[0]
print('dash:', hex(dash))

mids = find_middle_app()
print('middle-app candidates:')
for h, w, hh, t in mids:
    print('   ', hex(h), f'{w}x{hh}', repr(t[:50]))
mid = mids[0][0]

alt_tap_focus(mid)
time.sleep(1.5)

t0 = time.time()
u32.PostMessageW(mid, 0x0112, 0x020 * 1 + 6, 0)  # WM_SYSCOMMAND SC_MINIMIZE
rows = []
for i in range(40):
    rows.append((round((time.time() - t0) * 1000), zpos(dash), zpos(term)))
    time.sleep(0.1)

print('timeline (ms, dashZ, termZ):')
sank = None
recovered = None
for ms, dz, tz in rows:
    mark = ''
    if dz > tz and sank is None:
        sank = ms; mark = '  <-- dash BELOW term'
    if sank is not None and dz < tz and dz >= 0 and recovered is None:
        recovered = ms; mark = '  <-- guard recovered'
    print(f'   {ms:5d}  dash={dz:3d} term={tz:3d}{mark}')

print('RESULT: sank at', sank, 'ms; recovered at', recovered, 'ms')
u32.PostMessageW(dash, 0x0010, 0, 0)  # close dash
