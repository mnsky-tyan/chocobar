import os
# WizBar checkout to launch (set WIZBAR_HOME if not the default location)
WIZBAR = os.environ.get('WIZBAR_HOME', r'C:\path\to\wizbar')
ELECTRON = os.path.join(WIZBAR, 'node_modules', 'electron', 'dist', 'electron.exe')
import ctypes, subprocess, time, ctypes.wintypes
u32 = ctypes.windll.user32
u32.FindWindowW.restype = ctypes.c_void_p

ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

def find(pred):
    out = []
    def cb(h, _):
        c = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, c, 256)
        r = ctypes.wintypes.RECT()
        u32.GetWindowRect(h, ctypes.byref(r))
        if pred(c.value, h, r):
            out.append((h, r.left, r.top, r.right - r.left, r.bottom - r.top))
        return True
    u32.EnumWindows(ENUM(cb), None)
    return out

def dash_windows():
    # Electron dashboard: 840x580 chrome window (class matches electron)
    return find(lambda c, h, r: c == 'Chrome_WidgetWin_1' and (r.right - r.left) == 840 and (r.bottom - r.top) == 580)

def second_instance_summon():
    subprocess.run([ELECTRON, '.'],
                   cwd=WIZBAR,
                   capture_output=True, timeout=15)

print('step 1: summon dashboard')
second_instance_summon()
time.sleep(3)
d = dash_windows()
print('  dash windows:', d)

print('step 2: open notepad')
p = subprocess.Popen(['notepad.exe'])
time.sleep(3)
np = u32.FindWindowW(None, 'Untitled - Notepad') or u32.FindWindowW(None, 'Notepad')
print('  notepad hwnd:', np)

print('step 3: minimize notepad')
u32.ShowWindow(ctypes.c_void_p(np), 6)  # SW_MINIMIZE
time.sleep(2)
d = dash_windows()
vis = [x for x in d if u32.IsWindowVisible(ctypes.c_void_p(x[0]))]
print('  dash still exists:', bool(d), 'visible:', bool(vis))

print('step 4: close dashboard (WM_CLOSE)')
for h, *_ in d:
    u32.PostMessageW(ctypes.c_void_p(h), 0x0010, 0, 0)
time.sleep(2)
print('  dash after close:', dash_windows())

print('step 5: summon again')
second_instance_summon()
time.sleep(3)
d = dash_windows()
vis = [x for x in d if u32.IsWindowVisible(ctypes.c_void_p(x[0]))]
print('  dash recreated:', bool(d), 'visible:', bool(vis))

# kill the notepad we spawned
p.terminate()
