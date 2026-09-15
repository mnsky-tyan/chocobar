# Extract path-like strings from a byte range of the asar (route discovery).
import re, sys

data = open(r"C:\Users\tyanw\AppData\Local\Programs\Xiaomi MiMo AI\resources\app.asar", "rb").read()
region = data[16040000:16150000].decode("utf-8", "replace")
seen = set()
for m in re.finditer(r'"(/[A-Za-z0-9_][^"\s]{1,60})"', region):
    seen.add(m.group(1))
for m in re.finditer(r"'(/[A-Za-z0-9_][^'\s]{1,60})'", region):
    seen.add(m.group(1))
for p in sorted(seen):
    print(p)
