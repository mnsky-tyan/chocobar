import struct, zlib, os, math

W = H = 32
# pale yellow rounded square with pink diamond
px = [[(0, 0, 0, 0)] * W for _ in range(H)]

def rr(x, y, x0, y0, x1, y1, r):
    if x0 + r <= x <= x1 - r or y0 + r <= y <= y1 - r:
        return x0 <= x <= x1 and y0 <= y <= y1
    for cx in (x0 + r, x1 - r):
        for cy in (y0 + r, y1 - r):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                return True
    return False

for y in range(H):
    for x in range(W):
        if rr(x, y, 1, 1, 30, 30, 7):
            px[y][x] = (245, 240, 216, 255)  # #F5F0D8

cx, cy = 16, 16
for y in range(H):
    for x in range(W):
        # diamond |x-cx|+|y-cy| <= 8
        if abs(x - cx) + abs(y - cy) <= 8:
            # pink gradient toward deep pink at edges
            t = (abs(x - cx) + abs(y - cy)) / 8.0
            r = int(232 - t * (232 - 199))
            g = int(199 - t * (199 - 123))
            b = int(208 - t * (208 - 150))
            px[y][x] = (r, g, b, 255)

raw = b''.join(b'\x00' + bytes(v for p in row for v in p) for row in px)

def chunk(tag, data):
    c = struct.pack('>I', len(data)) + tag + data
    return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(raw, 9))
png += chunk(b'IEND', b'')

out = os.path.join(os.path.dirname(__file__), '..', 'assets', 'tray.png')
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, 'wb') as f:
    f.write(png)
print('wrote', os.path.abspath(out), len(png), 'bytes')
