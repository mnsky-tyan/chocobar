import ctypes, ctypes.wintypes
u32 = ctypes.windll.user32
u32.FindWindowW.restype = ctypes.c_void_p
h = u32.FindWindowW(None, 'Untitled - Notepad')
if not h:
    import subprocess
    # locate the top-level visible window of pid 15008
    ENUM = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(w, _):
        global h
        pid = ctypes.wintypes.DWORD()
        u32.GetWindowThreadProcessId(w, ctypes.byref(pid))
        if pid.value == 15008 and u32.IsWindowVisible(w):
            t = ctypes.create_unicode_buffer(64)
            u32.GetWindowTextW(w, t, 64)
            if t.value:
                h = w
        return True
    u32.EnumWindows(ENUM(cb), None)
print('hwnd', h)
if h:
    u32.PostMessageW(h, 0x0010, 0, 0)  # graceful WM_CLOSE (prompts if unsaved)
    print('posted WM_CLOSE')
