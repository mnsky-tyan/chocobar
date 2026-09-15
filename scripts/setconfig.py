"""wizbar config editor — uses the same comment-stripping as the app itself.
Usage: python setconfig.py key.subkey value [key2.subkey2 value2 ...]
"""
import json
import sys

P = os.path.join(os.path.expanduser('~'), '.wizbar', 'config.json')

def strip_comments(text):
    out = []
    in_str = False
    i = 0
    while i < len(text):
        c = text[i]
        if in_str:
            out.append(c)
            if c == '\\' and i + 1 < len(text):
                out.append(text[i + 1])
                i += 1
            elif c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == '/' and i + 1 < len(text) and text[i + 1] == '/':
            while i < len(text) and text[i] != '\n':
                i += 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)

cfg = json.loads(strip_comments(open(P, encoding='utf-8').read()))

args = sys.argv[1:]
for i in range(0, len(args), 2):
    path, val = args[i], args[i + 1]
    keys = path.split('.')
    node = cfg
    for k in keys[:-1]:
        node = node[k]
    old = node.get(keys[-1])
    try:
        val = json.loads(val)
    except Exception:
        pass
    node[keys[-1]] = val
    print(f'{path}: {old!r} -> {val!r}')

open(P, 'w', encoding='utf-8').write(json.dumps(cfg, indent=2))
