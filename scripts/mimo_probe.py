# Probe Xiaomi MiMo AI local sqlite stores for token/usage data (read-only).
import sqlite3, sys, os

base = os.path.join(os.environ['APPDATA'], 'Xiaomi MiMo AI')

for db in ['db/artifacts.db', 'db/rolechat.db', 'db/session-review.db']:
    p = os.path.join(base, db)
    print('=====', db)
    if not os.path.exists(p):
        print('  missing')
        continue
    try:
        con = sqlite3.connect('file:' + p.replace('\\', '/') + '?mode=ro', uri=True)
        for (name, sql) in con.execute("select name,sql from sqlite_master where type in ('table','view')"):
            print('--', name)
            print(sql)
        con.close()
    except Exception as e:
        print('ERR', e)
