"""Read turn_usage (+model attribution) from the zcode sqlite DB as JSON lines.
Usage: python zcode_query.py <db_path>
"""
import json
import sqlite3
import sys

db_path = sys.argv[1]
con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
cur = con.cursor()

try:
    models = {}
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
con.close()

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
