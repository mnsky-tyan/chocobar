# Extract context around needle occurrences in a binary file (asar bundle spelunking).
import sys, re, os

path = sys.argv[1]
needle = sys.argv[2].encode()
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 6
ctx = 260

data = open(path, 'rb').read()
start = 0
n = 0
while n < limit:
    i = data.find(needle, start)
    if i < 0:
        break
    lo = max(0, i - ctx)
    hi = min(len(data), i + len(needle) + ctx)
    chunk = data[lo:hi].decode('utf-8', 'replace').replace('\n', '\\n')
    print(f'--- @{i}')
    print(chunk)
    start = i + len(needle)
    n += 1
print(f'(total occurrences: {data.count(needle)})')
