from PIL import Image
import sys

img = Image.open('_screen.png').convert('RGB')
px = img.load()
x = int(sys.argv[1]) if len(sys.argv) > 1 else 430

def classify(r, g, b):
    if r > 215 and g > 200 and b > 170:
        return 'CREAM'
    if abs(r - g) < 18 and abs(g - b) < 25 and 110 < r <= 215:
        return 'GREY '
    return 'other'

prev = None
for y in range(0, 60):
    r, g, b = px[x, y]
    tag = classify(r, g, b)
    if tag != prev:
        print(f'y={y}: ({r},{g},{b}) {tag}')
        prev = tag
