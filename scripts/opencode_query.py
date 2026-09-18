"""Read assistant token usage from an opencode SQLite store as JSON lines.
Usage: python opencode_query.py <db_path>
Emits one JSON object per assistant message that carries a tokens object:
  { id, ts, model, input, output, reasoning, cacheRead, cacheWrite }
"""
import json
import sqlite3
import sys

db_path = sys.argv[1]

# mode=ro first; fall back to an immutable snapshot where a live -wal store
# rejects read-only opens (same workaround as zcode_query.py).
def read_rows(path):
    for uri in (f"file:{path}?mode=ro", f"file:{path}?immutable=1"):
        try:
            con = sqlite3.connect(uri, uri=True)
        except sqlite3.OperationalError:
            continue
        try:
            rows = con.execute(
                """SELECT id, session_id, time_created, data FROM message
                   WHERE data LIKE '%"tokens"%' AND time_created >= 0
                   ORDER BY time_created"""
            ).fetchall()
            models = {}
            try:
                for sid, model in con.execute("SELECT id, model FROM session"):
                    if not model:
                        continue
                    try:
                        mj = json.loads(model) if isinstance(model, str) else model
                        models[sid] = mj.get("id") or mj.get("modelID") or ""
                    except Exception:
                        models[sid] = str(model)
            except Exception:
                models = {}
            return rows, models
        except sqlite3.OperationalError:
            continue
        finally:
            con.close()
    return [], {}

rows, models = read_rows(db_path)
out = []
for (mid, sid, ts, data) in rows:
    try:
        msg = json.loads(data)
    except Exception:
        continue
    if not isinstance(msg, dict) or msg.get("role") != "assistant":
        continue
    t = msg.get("tokens")
    if not isinstance(t, dict):
        continue
    cache = t.get("cache") or {}
    rec = {
        "id": mid,
        "ts": ts,
        "model": msg.get("modelID") or (msg.get("model") or {}).get("modelID")
                 or (msg.get("model") or {}).get("modelId") or models.get(sid) or "unknown",
        "input": t.get("input") or 0,
        "output": t.get("output") or 0,
        "reasoning": t.get("reasoning") or 0,
        "cacheRead": cache.get("read") or 0,
        "cacheWrite": cache.get("write") or 0,
    }
    if rec["input"] or rec["output"] or rec["cacheRead"] or rec["cacheWrite"]:
        out.append(rec)
print(json.dumps(out))
