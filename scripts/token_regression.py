"""Execute token aggregation through the production Win32 scanner in isolation.

Windows: python scripts/token_regression.py --exe native/token-regression.exe
WSL: use a local-Windows-drive executable and the windowless win-run-hidden tool.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import uuid


def windows_path(path):
    if os.name == "nt":
        return str(path)
    return subprocess.check_output(["wslpath", "-w", str(path)], text=True, timeout=5).strip()


def fixture(root, name, seeded=False):
    profile = root / name
    configdir = profile / ".wizbar"
    sessions = profile / "sessions"
    configdir.mkdir(parents=True)
    sessions.mkdir()
    now = int(time.time() * 1000)
    fields = {"input": "prompt_tokens", "output": "completion_tokens",
              "cacheRead": "cache_read_tokens", "cacheWrite": "cache_write_tokens",
              "timestamp": "at", "model": "model_name"}
    config = {
        "tokens": {"enabled": True, "estimateMissingUsage": True, "sources": [
            {"app": "custom-cli", "path": windows_path(sessions),
             "enabled": True, "recursive": True, "fields": fields}]},
        "subs": {"enabled": False},
        "general": {"autoStart": False, "checkUpdates": False, "debug": False}}
    dump = lambda value: json.dumps(value, separators=(",", ":"))
    (configdir / "config.json").write_text(dump(config), encoding="utf-8")
    records = [
        {"prompt_tokens": 100, "completion_tokens": 40, "cache_read_tokens": 10,
         "cache_write_tokens": 0, "at": now - 1000, "model_name": "Model-A"},
        {"prompt_tokens": 80, "completion_tokens": 20, "cache_read_tokens": 10,
         "cache_write_tokens": 5, "at": now - 900, "model_name": "Model-B"}]
    (sessions / "usage.jsonl").write_text("".join(dump(r) + "\n" for r in records), encoding="utf-8")
    extra = {"prompt_tokens": 50, "completion_tokens": 5, "cache_read_tokens": 0,
             "cache_write_tokens": 0, "at": now - 800, "model_name": "Model-C"}
    (profile / "append-record.json").write_text(dump(extra) + "\n", encoding="utf-8")
    if seeded:
        seed = [["seed1", {"app": "seed-history", "ts": now - 5000, "input": 70,
                           "output": 20, "cacheRead": 0, "cacheWrite": 0, "model": "Seed-Model"}]]
        path = configdir / "token-cache.json"
        path.write_text(dump(seed), encoding="utf-8")
        # Preserve the production older-than-seed exclusion: the live fixture
        # really is newer than the history seed, including after its stamp moves.
        os.utime(path, ((now - 60000) / 1000, (now - 60000) / 1000))
        seed[0][1]["ts"] += 1000
        (profile / "seed-next.json").write_text(dump(seed), encoding="utf-8")
    return profile


def run_child(exe, profile, mode="normal", expected_exit=0):
    result = profile / ("result-" + uuid.uuid4().hex + ".json")
    args = [windows_path(exe), windows_path(profile), windows_path(result), mode]
    kwargs = {"text": True, "stdout": subprocess.PIPE, "stderr": subprocess.STDOUT,
              "stdin": subprocess.DEVNULL, "timeout": 45}
    if os.name == "nt":
        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
        command = args
    else:
        if not shutil.which("win-run-hidden"):
            raise RuntimeError("WSL requires win-run-hidden; no visible-process fallback")
        command = ["win-run-hidden", "--stdout", "--timeout", "25"] + args
    child = subprocess.run(command, **kwargs)
    if not result.is_file():
        raise AssertionError(f"Missing child completion file (launcher {child.returncode}): {child.stdout}")
    data = json.loads(result.read_text(encoding="utf-8"))
    if type(data.get("exit")) is not int or data["exit"] != expected_exit:
        raise AssertionError(f"Child status {data!r}, expected exit {expected_exit}")
    if os.name == "nt" and child.returncode != expected_exit:
        raise AssertionError(f"Process exit {child.returncode} does not match child status {data['exit']}")
    # WSL capture launchers may return zero on child failure. The fresh,
    # child-written completion file, never the wrapper's status, is the gate.
    return data["scans"]


def verify(scans, expected):
    if len(scans) != len(expected):
        raise AssertionError(f"Expected {len(expected)} scans, got {scans!r}")
    for i, (scan, (total, calls, models, warm)) in enumerate(zip(scans, expected)):
        observed = tuple(scan[k] for k in ("total", "app_total", "model_total", "calls", "models"))
        wanted = (total, total, total, calls, models)
        if observed != wanted:
            raise AssertionError(f"scan {i}: expected {wanted}, got {observed}")
        if warm and scan["bytes"] != 0:
            raise AssertionError(f"scan {i}: unchanged warm scan reread {scan['bytes']} bytes")
    return scans


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    options = parser.parse_args()
    exe = options.exe.resolve()
    if not exe.is_file():
        parser.error("compile the native console harness first")
    # WSL executions keep fixtures with the local Windows-drive executable.
    parent = exe.parent if os.name != "nt" else None
    failures = []
    def check(name, action):
        try:
            scans = action()
            print("PASS: " + name + " " + json.dumps(scans), flush=True)
        except (AssertionError, OSError, ValueError, subprocess.SubprocessError, RuntimeError) as error:
            failures.append(name)
            print(f"FAIL: {name}: {error}", flush=True)

    with tempfile.TemporaryDirectory(prefix="cb-token-", dir=parent) as folder:
        root = Path(folder)
        fresh = fixture(root, "fresh")
        seeded = fixture(root, "seeded", seeded=True)
        warm265 = [(265, 2, 2, False), (265, 2, 2, True)]
        warm355 = [(355, 3, 3, False), (355, 3, 3, True)]
        warm320 = [(320, 3, 3, False), (320, 3, 3, True)]
        # Invalid actual JSON exercises a real child failure and its propagation.
        invalid = fixture(root, "invalid")
        (invalid / ".wizbar" / "config.json").write_text("{", encoding="utf-8")
        check("child failure status is propagated", lambda: run_child(exe, invalid, expected_exit=3))
        check("seedless first start and unchanged warm scan", lambda: verify(run_child(exe, fresh), warm265))
        check("seedless restart preserves cards/apps/models/calls", lambda: verify(run_child(exe, fresh), warm265))
        check("append in-process preserves history", lambda: verify(run_child(exe, fresh, "append"),
              [(265, 2, 2, False), (320, 3, 3, False), (320, 3, 3, True)]))
        check("restart after append preserves history", lambda: verify(run_child(exe, fresh), warm320))
        check("seeded first start", lambda: verify(run_child(exe, seeded), warm355))
        check("seeded restart", lambda: verify(run_child(exe, seeded), warm355))
        check("seed reread with unchanged timestamp boundary", lambda: verify(run_child(exe, seeded, "refresh"),
              [(355, 3, 3, False), (355, 3, 3, False), (355, 3, 3, True)]))
        check("seed reread with moved timestamp boundary", lambda: verify(run_child(exe, seeded, "boundary"),
              [(355, 3, 3, False), (355, 3, 3, False), (355, 3, 3, True)]))
        toggled = fixture(root, "toggle")
        check("master off/on clears then rebuilds without doubling", lambda: verify(run_child(exe, toggled, "toggle"),
              [(265, 2, 2, False), (0, 0, 0, True), (265, 2, 2, False), (265, 2, 2, True)]))
        def source_toggle(name, partial=False):
            profile = fixture(root, name)
            path = profile / ".wizbar" / "config.json"
            config = json.loads(path.read_text(encoding="utf-8"))
            if partial:
                other = profile / "other-sessions"
                other.mkdir()
                shutil.copyfile(profile / "append-record.json", other / "usage.jsonl")
                config["tokens"]["sources"].append({
                    **config["tokens"]["sources"][0],
                    "app": "secondary-cli", "path": windows_path(other)})
                path.write_text(json.dumps(config), encoding="utf-8")
            baseline = warm320 if partial else warm265
            verify(run_child(exe, profile), baseline)  # A prior process persists EOF cursors.
            (profile / "config-on.json").write_text(json.dumps(config), encoding="utf-8")
            config["tokens"]["sources"][-1]["enabled"] = False
            path.write_text(json.dumps(config), encoding="utf-8")
            expected = ([(265, 2, 2, False), (320, 3, 3, False), (320, 3, 3, True)]
                        if partial else [(0, 0, 0, True), (265, 2, 2, False), (265, 2, 2, True)])
            return verify(run_child(exe, profile, "source-toggle"), expected)
        check("disabled source enabled after restart recovers history", lambda: source_toggle("source-off"))
        check("disabled source enabled alongside active source", lambda: source_toggle("source-partial", True))
        # Verify the cursor serialization as persisted output, not source text.
        def cursor_contract():
            data = json.loads((fresh / ".wizbar" / "token-cursors.json").read_text(encoding="utf-8"))
            path = windows_path(fresh / "sessions" / "usage.jsonl")
            row = data[path]
            if row["size"] != (fresh / "sessions" / "usage.jsonl").stat().st_size or row["chars"] != 0:
                raise AssertionError("cursor did not round-trip the file position and zero transcript length")
            return []
        check("cursor serialization round-trips escaped paths", cursor_contract)
    print(f"{13 - len(failures)}/13 executable token cases passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
