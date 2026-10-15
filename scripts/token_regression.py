"""Execute token aggregation through the production Win32 scanner in isolation.

The harness compiles the GENERATED amalgamation (native/src/chocobar_full.c);
build it with scripts/build_token_regression.sh, which refuses a stale one -
run `bash native/build.sh --check` first after any source edit.

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


def dump(value):
    return json.dumps(value, separators=(",", ":"))


def fixture(root, name, seeded=False, records=None):
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
    (configdir / "config.json").write_text(dump(config), encoding="utf-8")
    rows = records if records is not None else [
        {"prompt_tokens": 100, "completion_tokens": 40, "cache_read_tokens": 10,
         "cache_write_tokens": 0, "at": now - 1000, "model_name": "Model-A"},
        {"prompt_tokens": 80, "completion_tokens": 20, "cache_read_tokens": 10,
         "cache_write_tokens": 5, "at": now - 900, "model_name": "Model-B"}]
    (sessions / "usage.jsonl").write_text("".join(dump(r) + "\n" for r in rows), encoding="utf-8")
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


def run_exe(args):
    """Launch the console harness the platform-appropriate way.

    WSL cannot exec a Windows binary directly, and the launcher's own exit
    status does not carry the child's - probes print verdict lines instead.
    One launcher owns the subprocess shape so a new flag lands everywhere.
    """
    kwargs = {"text": True, "stdout": subprocess.PIPE, "stderr": subprocess.STDOUT,
              "stdin": subprocess.DEVNULL, "timeout": 45}
    if os.name == "nt":
        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
        command = args
    else:
        if not shutil.which("win-run-hidden"):
            raise RuntimeError("WSL requires win-run-hidden; no visible-process fallback")
        command = ["win-run-hidden", "--stdout", "--timeout", "25"] + args
    return subprocess.run(command, **kwargs)


def run_child(exe, profile, mode="normal", expected_exit=0):
    result = profile / ("result-" + uuid.uuid4().hex + ".json")
    args = [windows_path(exe), windows_path(profile), windows_path(result), mode]
    child = run_exe(args)
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
    for i, (scan, (total, calls, models, apps, warm)) in enumerate(zip(scans, expected)):
        # apps is pinned too: without it, a bug in aggRecord's app keying (two
        # apps merged by truncation, rows misattributed) leaves every case
        # green, because single-source fixtures cannot see WHERE rows landed.
        observed = tuple(scan[k] for k in ("total", "app_total", "model_total", "calls", "models", "apps"))
        wanted = (total, total, total, calls, models, apps)
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
    total = 0
    def check(name, action):
        nonlocal total
        total += 1
        try:
            scans = action()
            print("PASS: " + name + " " + json.dumps(scans), flush=True)
        # KeyError/TypeError included: a child JSON or cursor file missing an
        # expected key used to escape check() and abort the whole run with a
        # traceback instead of one counted FAIL line.
        except (AssertionError, OSError, ValueError, TypeError, KeyError,
                subprocess.SubprocessError, RuntimeError) as error:
            failures.append(name)
            print(f"FAIL: {name}: {error}", flush=True)

    with tempfile.TemporaryDirectory(prefix="cb-token-", dir=parent) as folder:
        root = Path(folder)
        fresh = fixture(root, "fresh")
        seeded = fixture(root, "seeded", seeded=True)
        warm265 = [(265, 2, 2, 1, False), (265, 2, 2, 1, True)]
        warm355 = [(355, 3, 3, 2, False), (355, 3, 3, 2, True)]
        warm320 = [(320, 3, 3, 1, False), (320, 3, 3, 1, True)]
        # Invalid actual JSON exercises a real child failure and its propagation.
        invalid = fixture(root, "invalid")
        (invalid / ".wizbar" / "config.json").write_text("{", encoding="utf-8")
        check("child failure status is propagated", lambda: run_child(exe, invalid, expected_exit=3))
        check("seedless first start and unchanged warm scan", lambda: verify(run_child(exe, fresh), warm265))
        check("seedless restart preserves cards/apps/models/calls", lambda: verify(run_child(exe, fresh), warm265))
        check("append in-process preserves history", lambda: verify(run_child(exe, fresh, "append"),
              [(265, 2, 2, 1, False), (320, 3, 3, 1, False), (320, 3, 3, 1, True)]))
        check("restart after append preserves history", lambda: verify(run_child(exe, fresh), warm320))
        check("seeded first start", lambda: verify(run_child(exe, seeded), warm355))
        check("seeded restart", lambda: verify(run_child(exe, seeded), warm355))
        check("seed reread with unchanged timestamp boundary", lambda: verify(run_child(exe, seeded, "refresh"),
              [(355, 3, 3, 2, False), (355, 3, 3, 2, False), (355, 3, 3, 2, True)]))
        check("seed reread with moved timestamp boundary", lambda: verify(run_child(exe, seeded, "boundary"),
              [(355, 3, 3, 2, False), (355, 3, 3, 2, False), (355, 3, 3, 2, True)]))
        toggled = fixture(root, "toggle")
        check("master off/on clears then rebuilds without doubling", lambda: verify(run_child(exe, toggled, "toggle"),
              [(265, 2, 2, 1, False), (0, 0, 0, 0, True), (265, 2, 2, 1, False), (265, 2, 2, 1, True)]))
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
            baseline = [(320, 3, 3, 2, False), (320, 3, 3, 2, True)] if partial else warm265
            verify(run_child(exe, profile), baseline)  # A prior process persists EOF cursors.
            (profile / "config-on.json").write_text(json.dumps(config), encoding="utf-8")
            config["tokens"]["sources"][-1]["enabled"] = False
            path.write_text(json.dumps(config), encoding="utf-8")
            expected = ([(265, 2, 2, 1, False), (320, 3, 3, 2, False), (320, 3, 3, 2, True)]
                        if partial else [(0, 0, 0, 0, True), (265, 2, 2, 1, False), (265, 2, 2, 1, True)])
            return verify(run_child(exe, profile, "source-toggle"), expected)
        check("disabled source enabled after restart recovers history", lambda: source_toggle("source-off"))
        check("disabled source enabled alongside active source", lambda: source_toggle("source-partial", True))

        # Route names that NEST ("mimo" inside "mimo-desktop", both real) must
        # be matched as exact comma-delimited elements, never as substrings: a
        # substring test read a blind turn on the shorter route as an aborted
        # reporting one and dropped it instead of estimating it, and refused to
        # record the shorter route in the reported list at all. Numbers, from
        # the default factors (inputFactor 1.10, outputFactor 1.04): the row-2
        # estimate is (40+60)/4*1.10 -> 28 prompt tokens and 60/4*1.04 -> 16
        # completion tokens, staged as 44; row 4 must NOT be estimated because
        # its route reported at row 3. 150 + 44 + 50 = 244, three calls, one model.
        now = int(time.time() * 1000)
        nested = fixture(root, "nested", records=[
            {"prompt_tokens": 100, "completion_tokens": 40, "cache_read_tokens": 10,
             "cache_write_tokens": 0, "at": now - 1000, "model_name": "Model-A",
             "provider": "mimo-desktop", "message": {"content": "a" * 40}},
            {"prompt_tokens": 0, "completion_tokens": 0, "cache_read_tokens": 0,
             "cache_write_tokens": 0, "at": now - 900, "model_name": "Model-A",
             "provider": "mimo", "message": {"content": "b" * 60}},
            {"prompt_tokens": 30, "completion_tokens": 10, "cache_read_tokens": 5,
             "cache_write_tokens": 5, "at": now - 800, "model_name": "Model-A",
             "provider": "mimo", "message": {"content": "c" * 20}},
            {"prompt_tokens": 0, "completion_tokens": 0, "cache_read_tokens": 0,
             "cache_write_tokens": 0, "at": now - 700, "model_name": "Model-A",
             "provider": "mimo", "message": {"content": "d" * 10}}])
        # Estimator state is cursor state: chars/real only exist once
        # tokens.estimateMissingUsage has been on for the file. Pin that the
        # whole-file list survives the cursor round-trip: the save side is
        # what the portable checks only pin as source text, and deleting it
        # would silently re-estimate aborted turns after every restart.
        def nested_cursor_real():
            scans = run_child(exe, nested)
            data = json.loads((nested / ".wizbar" / "token-cursors.json").read_text(encoding="utf-8"))
            row = data[windows_path(nested / "sessions" / "usage.jsonl")]
            if row["real"] != "mimo-desktop,mimo":
                raise AssertionError(f"cursor real did not round-trip: {row!r}")
            if row["chars"] != 130:  # 40 + 60 + 20 + 10 content chars
                raise AssertionError(f"cursor chars wrong: {row!r}")
            return scans
        check("nested route names are element-matched, not substring-matched",
              lambda: verify(nested_cursor_real(), [(244, 3, 1, 1, False), (244, 3, 1, 1, True)]))

        # A gateway that logs every call it serves (Magpie) and the client's
        # own session store BOTH hold the same request, so counting both
        # double-counts it. The routing is config: the gateway declares the
        # providers it owns, the session store excludes them. Measured
        # 2026-10-14 on the real ledgers: 2,893 requests both sides saw, and
        # every one reported identical numbers, so the union is exact - but
        # only if the exclude list actually suppresses the overlap.
        #
        # Three things are pinned at once: the ISO-8601 string timestamp is
        # decoded (epoch-ms parsers reject the whole record), the include
        # list admits a gateway record, and the exclude list suppresses the
        # client's copy of the SAME request. 100 + 200 + 30 = 330 counted
        # once; without the exclude list it would be 530.
        def gateway_union(name):
            profile = fixture(root, name)
            sessions = profile / "sessions"
            gateway = profile / "gateway"
            gateway.mkdir()
            # The client's copy of the same two requests, plus one the
            # gateway never saw (a direct provider, which must still count).
            rows = [
                {"prompt_tokens": 100, "completion_tokens": 40, "cache_read_tokens": 10,
                 "cache_write_tokens": 0, "at": now - 1000, "model_name": "Model-A",
                 "provider": "magpie-qoder"},
                {"prompt_tokens": 200, "completion_tokens": 60, "cache_read_tokens": 20,
                 "cache_write_tokens": 5, "at": now - 900, "model_name": "Model-B",
                 "provider": "magpie-cline"},
                {"prompt_tokens": 30, "completion_tokens": 10, "cache_read_tokens": 5,
                 "cache_write_tokens": 0, "at": now - 800, "model_name": "Model-C",
                 "provider": "zcode"}]
            (sessions / "usage.jsonl").write_text(
                "".join(dump(r) + "\n" for r in rows), encoding="utf-8")
            # The gateway's own ledger: the same two requests (identical
            # numbers) plus one subagent call the client never logged, in
            # Magpie's real shape - a string timestamp with a +08:00 offset
            # and the short field names.
            stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime((now - 1000) / 1000))
            gw = [
                {"t": stamp + ".123456789+08:00", "provider": "qoder", "model": "Model-A",
                 "in": 100, "out": 40, "cache_read": 10, "cache_write": 0, "status": 200},
                {"t": stamp + ".223456789+08:00", "provider": "cline", "model": "Model-B",
                 "in": 200, "out": 60, "cache_read": 20, "cache_write": 5, "status": 200},
                {"t": stamp + ".323456789+08:00", "provider": "qoder", "model": "Model-D",
                 "in": 400, "out": 100, "cache_read": 50, "cache_write": 0, "status": 200}]
            (gateway / "usage.jsonl").write_text(
                "".join(dump(r) + "\n" for r in gw), encoding="utf-8")
            path = profile / ".wizbar" / "config.json"
            config = json.loads(path.read_text(encoding="utf-8"))
            client = config["tokens"]["sources"][0]
            client["exclude"] = ["magpie-qoder", "magpie-cline"]
            config["tokens"]["sources"].append({
                "app": "magpie", "path": windows_path(gateway), "enabled": True,
                "recursive": False, "timestampFormat": "iso",
                "fields": {"input": "in", "output": "out", "cacheRead": "cache_read",
                           "cacheWrite": "cache_write", "timestamp": "t",
                           "model": "model"},
                "providers": ["qoder", "cline"]})
            path.write_text(json.dumps(config), encoding="utf-8")
            # 100 + 40 + 10 (qoder, counted once) + 200 + 60 + 25 (cline, once)
            # + 30 + 10 + 5 (zcode, direct) + 400 + 100 + 50 (the subagent
            # call only the gateway saw) = 1030, five calls, four models.
            return verify(run_child(exe, profile),
                          [(1030, 4, 4, 2, False), (1030, 4, 4, 2, True)])
        check("gateway ledger and session store count one request once",
              lambda: gateway_union("gw-union"))
        # The mirror image: a provider the gateway does NOT own must fall
        # through to the client's number, so a narrow include list cannot
        # silently drop a whole route from the bar.
        def gateway_filtered(name):
            profile = fixture(root, name)
            sessions = profile / "sessions"
            gateway = profile / "gateway"
            gateway.mkdir()
            rows = [
                {"prompt_tokens": 100, "completion_tokens": 40, "cache_read_tokens": 10,
                 "cache_write_tokens": 0, "at": now - 1000, "model_name": "Model-A",
                 "provider": "magpie-qoder"},
                {"prompt_tokens": 30, "completion_tokens": 10, "cache_read_tokens": 5,
                 "cache_write_tokens": 0, "at": now - 800, "model_name": "Model-C",
                 "provider": "zcode"}]
            (sessions / "usage.jsonl").write_text(
                "".join(dump(r) + "\n" for r in rows), encoding="utf-8")
            stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime((now - 1000) / 1000))
            gw = [{"t": stamp + ".123456789+08:00", "provider": "unlisted-route",
                   "model": "Model-Z", "in": 999, "out": 999, "cache_read": 0,
                   "cache_write": 0, "status": 200}]
            (gateway / "usage.jsonl").write_text(
                "".join(dump(r) + "\n" for r in gw), encoding="utf-8")
            path = profile / ".wizbar" / "config.json"
            config = json.loads(path.read_text(encoding="utf-8"))
            client = config["tokens"]["sources"][0]
            client["exclude"] = ["magpie-qoder"]
            config["tokens"]["sources"].append({
                "app": "magpie", "path": windows_path(gateway), "enabled": True,
                "recursive": False, "timestampFormat": "iso",
                "fields": {"input": "in", "output": "out", "cacheRead": "cache_read",
                           "cacheWrite": "cache_write", "timestamp": "t",
                           "model": "model"},
                "providers": ["qoder", "cline"]})
            path.write_text(json.dumps(config), encoding="utf-8")
            # The gateway's unlisted route is NOT counted (999+999 dropped by
            # the include list) and the client's copy of magpie-qoder is NOT
            # counted either (excluded), so only zcode's 30+10+5 survives.
            return verify(run_child(exe, profile),
                          [(45, 1, 1, 1, False), (45, 1, 1, 1, True)])
        check("gateway include list drops what it does not own, exclude keeps the rest",
              lambda: gateway_filtered("gw-filter"))

        # The provider match folds case, like every sibling key in the pipeline
        # (app allowlist, label lookup, model key, timestampFormat). A config
        # that spells a route differently from the ledger must still route:
        # a case-sensitive match would silently drop the whole gateway route
        # (an include list) or silently re-enable the double-count (an exclude
        # list). Config says "QODER"/"Magpie-Cline", the ledger says "qoder"/
        # "magpie-cline".
        def gateway_casefold(name):
            profile = fixture(root, name)
            sessions = profile / "sessions"
            gateway = profile / "gateway"
            gateway.mkdir()
            rows = [
                {"prompt_tokens": 100, "completion_tokens": 40, "cache_read_tokens": 10,
                 "cache_write_tokens": 0, "at": now - 1000, "model_name": "Model-A",
                 "provider": "magpie-qoder"},
                {"prompt_tokens": 30, "completion_tokens": 10, "cache_read_tokens": 5,
                 "cache_write_tokens": 0, "at": now - 800, "model_name": "Model-C",
                 "provider": "zcode"}]
            (sessions / "usage.jsonl").write_text(
                "".join(dump(r) + "\n" for r in rows), encoding="utf-8")
            stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime((now - 1000) / 1000))
            gw = [{"t": stamp + ".123456789+08:00", "provider": "qoder",
                   "model": "Model-A", "in": 100, "out": 40, "cache_read": 10,
                   "cache_write": 0, "status": 200}]
            (gateway / "usage.jsonl").write_text(
                "".join(dump(r) + "\n" for r in gw), encoding="utf-8")
            path = profile / ".wizbar" / "config.json"
            config = json.loads(path.read_text(encoding="utf-8"))
            client = config["tokens"]["sources"][0]
            client["exclude"] = ["MAGPIE-QODER"]          # different case on purpose
            config["tokens"]["sources"].append({
                "app": "magpie", "path": windows_path(gateway), "enabled": True,
                "recursive": False, "timestampFormat": "iso",
                "fields": {"input": "in", "output": "out", "cacheRead": "cache_read",
                           "cacheWrite": "cache_write", "timestamp": "t",
                           "model": "model"},
                "providers": ["Qoder"]})              # different case on purpose
            path.write_text(json.dumps(config), encoding="utf-8")
            # qoder counted once (100+40+10) + zcode direct (30+10+5) = 195.
            # A case-sensitive match would give 105 (gateway route dropped by
            # the include list AND the client copy still excluded).
            return verify(run_child(exe, profile),
                          [(195, 2, 2, 2, False), (195, 2, 2, 2, True)])
        check("gateway provider routing folds case on both lists",
              lambda: gateway_casefold("gw-casefold"))

        # A file first scanned while the estimator was OFF has no transcript
        # total in its cursor. Switching the estimator on mid-run must repair
        # that cursor WITHOUT re-counting the file: the re-read establishes
        # the total, staging stops at the old cursor, and only the record
        # appended while the estimator was off is newly counted. The old code
        # re-staged the whole file and doubled the totals (585 here).
        def estimate_toggle(name):
            profile = fixture(root, name)
            path = profile / ".wizbar" / "config.json"
            config = json.loads(path.read_text(encoding="utf-8"))
            config["tokens"]["estimateMissingUsage"] = False
            path.write_text(json.dumps(config), encoding="utf-8")
            on = json.loads(path.read_text(encoding="utf-8"))
            on["tokens"]["estimateMissingUsage"] = True
            (profile / "config-on.json").write_text(json.dumps(on), encoding="utf-8")
            return verify(run_child(exe, profile, "estimate-toggle"),
                          [(265, 2, 2, 1, False), (320, 3, 3, 1, False), (320, 3, 3, 1, True)])
        check("enabling the estimator mid-run repairs the cursor without re-counting",
              lambda: estimate_toggle("est-off"))
        # Verify the cursor serialization as persisted output, not source text.
        def cursor_contract():
            data = json.loads((fresh / ".wizbar" / "token-cursors.json").read_text(encoding="utf-8"))
            path = windows_path(fresh / "sessions" / "usage.jsonl")
            row = data[path]
            if row["size"] != (fresh / "sessions" / "usage.jsonl").stat().st_size or row["chars"] != 0:
                raise AssertionError("cursor did not round-trip the file position and zero transcript length")
            return []
        check("cursor serialization round-trips escaped paths", cursor_contract)
        # The antigravity provider must not invent a credential path. The child
        # plants a plausible vendor store plus a second user-named store in its
        # own scratch profile and asserts the reader resolves nothing when the
        # config names no path, and reads exactly the named file when it does.
        def agy_no_default_path():
            profile = fixture(root, "agy-path")
            child = run_exe([windows_path(exe), "--agy-probe", windows_path(profile)])
            text = child.stdout or ""
            if os.name == "nt" and child.returncode != 0:
                raise AssertionError(f"agy probe exited {child.returncode}: {text}")
            verdicts = {}
            for line in text.splitlines():
                label, sep, rest = line.partition(" found=")
                if not sep:
                    continue
                found, _, resolved = rest.partition(" resolved=\"")
                if found not in ("0", "1") or not resolved.endswith("\""):
                    raise AssertionError(f"agy probe line did not parse: {line}")
                if label in verdicts:
                    raise AssertionError(f"agy probe repeated {label}: {text}")
                verdicts[label] = (found == "1", resolved[:-1])
            wanted = {"empty_authPath": False, "absent_authPath": False, "explicit_own": True}
            if set(verdicts) != set(wanted):
                raise AssertionError(f"agy probe did not report its cases: {text}")
            for label, want_found in wanted.items():
                found, resolved = verdicts[label]
                if found != want_found:
                    raise AssertionError(f"{label}: expected found={int(want_found)}: {text}")
                if ".pi" in resolved:
                    raise AssertionError(f"{label}: resolved to the vendor store: {resolved}")
                if want_found and not (".wizbar" in resolved and "antigravity.json" in resolved):
                    raise AssertionError(f"{label}: {resolved} is not the explicit user path")
            return [line for line in text.splitlines() if line.strip()]
        check("antigravity never invents a credential path", agy_no_default_path)

        # Omitting a key must keep its built-in default (the template promises
        # "delete a key and the built-in default applies").
        def omitted_keys_keep_defaults():
            child = run_exe([windows_path(exe), "--defaults-probe"])
            text = child.stdout or ""
            if os.name == "nt" and child.returncode != 0:
                raise AssertionError(f"defaults probe exited {child.returncode}: {text}")
            seen = {}
            for line in text.splitlines():
                key, sep, value = line.partition("=")
                if sep and key in ("backgroundTint", "backdrop", "fontFamily", "verdict"):
                    seen[key] = value.strip()
            wanted = {"backgroundTint": "#FBF2E2", "backdrop": "acrylic", "fontFamily": "Cascadia Mono"}
            for key, want in wanted.items():
                if seen.get(key) != want:
                    raise AssertionError(f"{key}: expected {want}, got {seen.get(key)!r}: {text}")
            if seen.get("verdict") != "DEFAULTS_KEPT":
                raise AssertionError(f"defaults were lost when keys were omitted: {text}")
            return [line for line in text.splitlines() if line.strip()]
        check("omitted config keys keep their built-in defaults", omitted_keys_keep_defaults)
    print(f"{total - len(failures)}/{total} executable token cases passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
