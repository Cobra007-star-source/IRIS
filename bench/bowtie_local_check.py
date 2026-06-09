#!/usr/bin/env python3
# =============================================================================
# bench/bowtie_local_check.py
#
# Local Bowtie/IHOP driver. Exercises the IRIS Bowtie harness through the exact
# stdio protocol Bowtie uses, but without Docker or the bowtie CLI, so we can
# confirm the harness reproduces IRIS's conformance on a plain dev box.
#
# It speaks the protocol to ./build*/bench/bowtie_iris:
#   start -> dialect -> run (one per test case) -> stop
# and compares each test's reported `valid` against the official suite's
# expected value, printing pass/fail/errored tallies per file and overall.
#
# The inline `registry` of every run command is loaded with ALL remote schemas
# from .test-suite/remotes (uri = http://localhost:1234/<relpath>), mirroring
# how Bowtie supplies $ref targets. The harness only consumes the registry for
# cases that route to the slow path, so fast-path cases are unaffected.
#
# Usage:
#   python3 bench/bowtie_local_check.py [harness_bin] [tests_dir] [--verbose]
#   (defaults: build-bowtie/bench/bowtie_iris, .test-suite/tests/draft2020-12)
# =============================================================================
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") \
    else os.path.join(ROOT, "build-bowtie/bench/bowtie_iris")
TESTS_DIR = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") \
    else os.path.join(ROOT, ".test-suite/tests/draft2020-12")
REMOTES_DIR = os.path.join(ROOT, ".test-suite/remotes")
VERBOSE = "--verbose" in sys.argv
DIALECT = "https://json-schema.org/draft/2020-12/schema"


def load_registry():
    reg = {}
    for dirpath, _, files in os.walk(REMOTES_DIR):
        for f in files:
            if not f.endswith(".json"):
                continue
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, REMOTES_DIR).replace(os.sep, "/")
            try:
                with open(full) as fh:
                    reg["http://localhost:1234/" + rel] = json.load(fh)
            except json.JSONDecodeError:
                pass  # a few remotes are intentionally malformed fixtures
    return reg


def main():
    proc = subprocess.Popen(
        [HARNESS], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        text=True, bufsize=1,
    )

    def cmd(obj):
        proc.stdin.write(json.dumps(obj) + "\n")
        proc.stdin.flush()
        return json.loads(proc.stdout.readline())

    started = cmd({"cmd": "start", "version": 1})
    assert started.get("version") == 1, started
    dialect = cmd({"cmd": "dialect", "dialect": DIALECT})
    assert dialect.get("ok") is True, dialect

    registry = load_registry()

    files = sorted(f for f in os.listdir(TESTS_DIR) if f.endswith(".json"))
    tot = {"total": 0, "pass": 0, "fail": 0, "errored": 0}
    seq = 0
    print(f"{'file':32} | {'tot':>4} | {'pass':>4} | {'fail':>4} | {'err':>4}")
    print("-" * 64)
    for fname in files:
        with open(os.path.join(TESTS_DIR, fname)) as fh:
            cases = json.load(fh)
        f_tot = f_pass = f_fail = f_err = 0
        for case in cases:
            seq += 1
            tests = case.get("tests", [])
            run = {
                "cmd": "run", "seq": seq,
                "case": {
                    "description": case.get("description", ""),
                    "schema": case["schema"],
                    "registry": registry,
                    "tests": [{"instance": t["data"]} for t in tests],
                },
            }
            resp = cmd(run)
            results = resp.get("results", [])
            for i, t in enumerate(tests):
                f_tot += 1
                expected = t["valid"]
                r = results[i] if i < len(results) else {}
                if r.get("errored") or r.get("skipped"):
                    f_err += 1
                    if VERBOSE:
                        print(f"  ERR  [{fname}] {case.get('description')} :: "
                              f"{t.get('description')} :: {r}", file=sys.stderr)
                    continue
                got = bool(r.get("valid"))
                if got == expected:
                    f_pass += 1
                else:
                    f_fail += 1
                    if VERBOSE:
                        print(f"  FAIL [{fname}] {case.get('description')} :: "
                              f"{t.get('description')} :: expect={expected} got={got}",
                              file=sys.stderr)
        print(f"{fname:32} | {f_tot:>4} | {f_pass:>4} | {f_fail:>4} | {f_err:>4}")
        tot["total"] += f_tot
        tot["pass"] += f_pass
        tot["fail"] += f_fail
        tot["errored"] += f_err

    cmd_stop = {"cmd": "stop"}
    proc.stdin.write(json.dumps(cmd_stop) + "\n")
    proc.stdin.flush()
    proc.wait(timeout=5)

    print("-" * 64)
    print(f"{'TOTAL':32} | {tot['total']:>4} | {tot['pass']:>4} | "
          f"{tot['fail']:>4} | {tot['errored']:>4}")
    if tot["total"]:
        rate = 100.0 * tot["pass"] / tot["total"]
        print(f"\nraw pass rate: {tot['pass']}/{tot['total']} = {rate:.2f}%")
    return 0 if tot["fail"] == 0 and tot["errored"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
