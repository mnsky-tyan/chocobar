# Repro + assert for the dashboard z-order bug:
#   summon dash -> foreground another app -> minimize it (the storm) -> the dash
#   must stay ABOVE the terminal and near the top of the z-stack.
# Reports z-indices from EnumWindows (top->bottom order), which needs no
# DPI awareness (no coordinates involved).
import ctypes, subprocess, time, ctypes.wintypes, json, os

u32 = ctypes.windll.user32
u32.FindWindowW.restype = ctypes.c_void_p
u32.GetForegroundWindow.restype = ctypes.c_void_p

ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

def zlist():
    hwnds = []
    def cb(h, _):
        hwnds.append(h)
        return True
    u32.EnumWindows(ENUM(cb), None)
    return hwnds

def info(h):
    c = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, c, 256)
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, t, 256)
    return f"{c.value}|{t.value[:30]}"

def positions(label, want_names):
    zs = zlist()
    fg = u32.GetForegroundWindow()
    out = {}
    for h in want_names:
        try:
            out[h] = zs.index(h)
        except ValueError:
            out[h] = -1
    print(f"[{label}] z-positions (0=top):", {info(k): v for k, v in out.items()},
          "| dash_fg:", fg in want_names)
    return out

def find_terminals():
    out = []
    def cb(h, _):
        c = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, c, 256)
        if c.value == 'CASCADIA_HOSTING_WINDOW_CLASS' and u32.IsWindowVisible(h) \
                and u32.GetWindowTextLengthW(h) > 0 and not u32.IsIconic(h):
            out.append(h)
        return True
    u32.EnumWindows(ENUM(cb), None)
    return out

def find_dash():
    out = []
    def cb(h, _):
        c = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, c, 256)
        r = ctypes.wintypes.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        if c.value == 'Chrome_WidgetWin_1' and (r.right - r.left) == 840 and (r.bottom - r.top) == 580 \
                and u32.IsWindowVisible(h):
            out.append(h)
        return True
    u32.EnumWindows(ENUM(cb), None)
    return out

def find_foreground_app():
    # any visible top-level window that is NOT the terminal and NOT electron dash:
    # use the Brave/browser window if present, else spawn notepad
    out = []
    def cb(h, _):
        c = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, c, 256)
        if c.value in ('Chrome_WidgetWin_1',) and u32.IsWindowVisible(h) and not u32.IsIconic(h):
            r = ctypes.wintypes.RECT()
            u32.GetWindowRect(h, ctypes.byref(r))
            if (r.right - r.left) != 840 and u32.GetWindowTextLengthW(h) > 0:
                t = ctypes.create_unicode_buffer(256)
                u32.GetWindowTextW(h, t, 256)
                if 'Brave' in t.value or 'YouTube' in t.value or 'Chrome' in t.value:
                    out.append(h)
        return True
    u32.EnumWindows(ENUM(cb), None)
    return out

def second_instance_summon():
    subprocess.run([r'C:\Users\tyanw\work\general\wizbar\node_modules\electron\dist\electron.exe', '.'],
                   cwd=r'C:\Users\tyanw\work\general\wizbar', capture_output=True, timeout=15)

terms = find_terminals()
print("terminals:", [(hex(t), info(t)) for t in terms])
term = terms[0]

print("step 1: summon dashboard")
second_instance_summon()
time.sleep(3)
dashes = find_dash()
print("  dash:", [hex(d) for d in dashes])
dash = dashes[0]
positions("after summon", [dash, term])

print("step 2: foreground the browser, then minimize it (the storm)")
brave = find_foreground_app()
victim = None
if brave:
    victim = brave[0]
    print("  using existing browser window:", hex(victim), info(victim))
    u32.SetForegroundWindow(victim)
    time.sleep(1.0)
else:
    print("  no browser found; spawning notepad")
    p = subprocess.Popen(['notepad.exe'])
    time.sleep(2.5)
    victim = u32.FindWindowW(None, 'Untitled - Notepad') or u32.FindWindowW(None, 'Notepad')
positions("victim foregrounded", [dash, term, victim])

u32.ShowWindow(victim, 6)  # SW_MINIMIZE
time.sleep(2.0)  # guard runs every 350ms
pos = positions("AFTER minimize-storm", [dash, term])

ok = pos[dash] >= 0 and pos[term] >= 0 and pos[dash] < pos[term]
print("ASSERT dash above terminal:", "PASS" if ok else "FAIL")

print("step 3: cleanup — close dash, restore victim")
u32.PostMessageW(dash, 0x0010, 0, 0)
u32.ShowWindow(victim, 9)  # SW_RESTORE
time.sleep(1)
print("done")
