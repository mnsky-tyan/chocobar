# Decisive z-order experiment: does SetWindowPos(dash, term) actually move the
# dash per EnumWindows? Try repeatedly, with owner/cloak/iconic dumps and
# GetLastError on every call.
import ctypes, ctypes.wintypes, time

u32 = ctypes.windll.user32
u32.SetWindowPos.restype = ctypes.c_bool
u32.GetWindow.restype = ctypes.c_void_p
u32.GetAncestor.restype = ctypes.c_void_p
dwm = ctypes.windll.dwmapi
ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

order = []

def collect(h, _):
    order.append(h)
    return True

COLLECT_CB = ENUM(collect)  # keep-alive for the whole script

def zscan():
    order.clear()
    u32.EnumWindows(COLLECT_CB, None)
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

def find():
    dash = term = None
    for h in zscan():
        if cls(h) == 'CASCADIA_HOSTING_WINDOW_CLASS' and title(h) and term is None:
            term = h
        if cls(h) == 'Chrome_WidgetWin_1' and title(h) == 'WizBar' and rect(h)[2:] == (840, 580):
            dash = h
    return dash, term

def dump(label, dash, term):
    for name, h in (('dash', dash), ('term', term)):
        vis = bool(u32.IsWindowVisible(h))
        ico = bool(u32.IsIconic(h))
        cloaked = ctypes.c_int(0)
        dwm.DwmGetWindowAttribute(h, 14, ctypes.byref(cloaked), 4)
        owner = u32.GetAncestor(h, 4) or 0  # GA_ROOTOWNER (0 = none)
        idx = order.index(h) if h in order else -1
        print(f'  {label} {name}: idx={idx} hwnd={hex(h)} visible={vis} iconic={ico} cloaked={cloaked.value} rootowner={hex(owner)} rect={rect(h)}')

dash, term = find()
if not dash or not term:
    print('dash or term not found (dash=', dash, ' term=', term, ') — is the dashboard open?')
    raise SystemExit

dump('t0', dash, term)

NOMOVE, NOSIZE, NOACTIVATE = 0x2, 0x1, 0x10
for attempt in range(1, 5):
    ok = u32.SetWindowPos(dash, term, 0, 0, 0, 0, NOMOVE | NOSIZE | NOACTIVATE)
    err = ctypes.get_last_error()
    time.sleep(0.25)
    zscan()
    di = order.index(dash) if dash in order else -1
    ti = order.index(term) if term in order else -1
    print(f'attempt {attempt}: SetWindowPos={ok} lasterr={err} -> dash idx={di} term idx={ti} dash_above={di != -1 and ti != -1 and di < ti}')
    if attempt == 2 and di == -1:
        print('  dash LEFT the enumeration after raise?! re-listing WizBar-titled windows:')
        for h in order:
            if title(h) == 'WizBar':
                print('   ', hex(h), cls(h), rect(h), 'visible=', bool(u32.IsWindowVisible(h)))

dump('t1', dash, term)
