import json

p = os.path.join(os.path.expanduser('~'), '.wizbar', 'config.json')
w = json.load(open(p))
w['bar']['gap'] = 16
w['bar']['roundCorners'] = True
w['bar']['backgroundAlpha'] = 180
w['bar']['insetX'] = 2
w['bar']['backdrop'] = 'acrylic'
w['general']['debug'] = False
json.dump(w, open(p, 'w'), indent=2)
print('wizbar config: gap=16 roundCorners=True alpha=180 insetX=2')
