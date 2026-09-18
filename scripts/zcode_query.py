"""Read turn_usage (+model attribution) from the zcode sqlite DB as JSON lines.
Usage: python zcode_query.py <db_path>
"""
import json
import sqlite3
import sys

db_path = sys.argv[1]

# Open read-only. mode=ro is the correct mode for a live store; some
# filesystems (WSL drvfs with an active -wal) accept the connect but fail
# mid-query with a transient 'disk I/O error'. Fall back to an immutable
# snapshot there — it reads only the main db file (uncheckpointed -wal rows
# may be missing) instead of failing the whole scan.
def read_rows(path, immutable=False):
    uri = f"file:{path}?immutable=1" if immutable else f"file:{path}?mode=ro"
    con = sqlite3.connect(uri, uri=True)
    try:
        cur = con.cursor()
        models = {}
        try:
            for turn_id, model_id in cur.execute(
                "SELECT turn_id, model_id FROM model_usage ORDER BY completed_at ASC"
            ):
                models[turn_id] = model_id
        except Exception:
            models = {}
        rows = cur.execute(
            """
            SELECT session_id, turn_id, completed_at,
                   input_tokens, output_tokens, reasoning_tokens,
                   cache_creation_input_tokens, cache_read_input_tokens
            FROM turn_usage
            WHERE completed_at > 0
            """
        ).fetchall()
        return models, rows
    finally:
        con.close()

try:
    models, rows = read_rows(db_path)
except sqlite3.OperationalError:
    models, rows = read_rows(db_path, immutable=True)

out = []
for (sid, tid, ts, inp, outp, reas, cw, cr) in rows:
    out.append({
        "session": sid,
        "turn": tid,
        "ts": ts,
        "input": inp or 0,
        "output": outp or 0,
        "reasoning": reas or 0,
        "cacheWrite": cw or 0,
        "cacheRead": cr or 0,
        "model": models.get(tid, "unknown"),
    })
print(json.dumps(out))
