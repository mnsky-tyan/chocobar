# DPI-aware native-resolution grab of the taskbar tray corner.
import ctypes
ctypes.windll.user32.SetProcessDPIAware()
from PIL import Image, ImageGrab

sw = ctypes.windll.user32.GetSystemMetrics(0)  # physical after DPI-aware
sh = ctypes.windll.user32.GetSystemMetrics(1)
box = (sw - 560, sh - 100, sw, sh)
img = ImageGrab.grab(bbox=box, all_screens=True)
img = img.resize((img.width * 2, img.height * 2), Image.LANCZOS)  # 2x for legibility
out = r'C:\Users\tyanw\work\general\wizbar\_tray_corner.png'
img.save(out)
print('saved', out, img.size, 'screen', sw, sh)
