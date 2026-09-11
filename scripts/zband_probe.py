# Deep z-order probe: full EnumWindows order around dash+term, WS_EX_TOPMOST
# styles, then a manual SetWindowPos and re-read.
import ctypes, ctypes.wintypes

u32 = ctypes.windll.user32
u32.SetWindowPos.restype = ctypes.c_bool
u32.GetWindowLongW.restype = ctypes.c_long
ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

order = []

def cb(h, _):
    order.append(h)
    return True

u32.EnumWindows(ENUM(cb), None)

term = None
dash = None
bar = None

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
    return (r.right - r.left, r.bottom - r.top)

for i, h in enumerate(order):
    c = cls(h)
    if c == 'CASCADIA_HOSTING_WINDOW_CLASS' and title(h) and term is None:
        term = (i, h)
    if c == 'Chrome_WidgetWin_1' and rect(h) == (840, 580) and title(h) == 'WizBar':
        dash = (i, h)
    if c == 'Chrome_WidgetWin_1' and rect(h)[1] == 36 and title(h) == 'WizBar':
        bar = (i, h)

GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x8

for name, entry in (('dash', dash), ('term', term), ('bar', bar)):
    if not entry:
        print(name, 'NOT FOUND')
        continue
    i, h = entry
    ex = u32.GetWindowLongW(h, GWL_EXSTYLE)
    print(f'{name}: idx={i} hwnd={hex(h)} topmost={bool(ex & WS_EX_TOPMOST)} exstyle={hex(ex & 0xFFFFFFFF)}')

di, dh = dash
ti, th = term
print('dash above term (idx<):', di < ti)

# neighborhood: 3 windows above and below the dash in the enumeration
print('\nneighborhood of dash:')
for j in range(max(0, di - 3), min(len(order), di + 4)):
    mark = ' <-- DASH' if j == di else (' <-- TERM' if j == ti else '')
    print(f'   idx={j} {cls(order[j])[:34]!r} {title(order[j])[:34]!r} {rect(order[j])}{mark}')

ok = u32.SetWindowPos(dh, th, 0, 0, 0, 0, 0x2 | 0x1 | 0x10)  # NOMOVE|NOSIZE|NOACTIVATE, insert after term
print('\nmanual SetWindowPos(dash after term):', ok)
import time
time.sleep(0.3)
order2 = []
u32.EnumWindows(ENUM(lambda h, _: (order2.append(h), True)[1]), None)
di2 = order2.index(dh) if dh in order2 else -1
ti2 = order2.index(th) if th in order2 else -1
print(f'after raise: dash idx={di2}, term idx={ti2}, dash above term: {di2 < ti2}')
