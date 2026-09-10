from PIL import Image
import sys

img = Image.open('_screen.png').convert('RGB')
px = img.load()
x = int(sys.argv[1]) if len(sys.argv) > 1 else 700
for y in range(0, 50):
    r, g, b = px[x, y]
    if r > 215 and g > 200 and b > 170:
        tag = 'CREAM'
    elif abs(r - g) < 18 and abs(g - b) < 25 and 110 < r <= 215:
        tag = 'GREY '
    else:
        tag = 'other'
    print(y, (r, g, b), tag)
