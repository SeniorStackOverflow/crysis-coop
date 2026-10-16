"""Says what went wrong in a game, from its logs.

  python triage.py <Game.log | host log | run folder> [companion.log]
                   [--window FIRST:LAST] [--map Coop.map] [--json out.json] [--ru out.md]

A run folder is one with at_host.log / *host*.log (and at_companion.log /
*companion*.log). Exit code: 0 nothing wrong, 1 a FAIL, 2 a CRASH, 3 INFRA only.
"""

import argparse
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from coopjudge.judge import judge            # noqa: E402
from coopjudge.logread import Log            # noqa: E402
from coopjudge.report import results_json, summary_ru  # noqa: E402


def find_logs(path):
    if os.path.isdir(path):
        def one(pats):
            for p in pats:
                hits = sorted(glob.glob(os.path.join(path, p)))
                if hits:
                    return hits[0]
            return None
        return one(["at_host.log", "*host*.log", "Game.log"]), one(["at_companion.log", "*companion*.log", "companion.log"])
    return path, None


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("companion", nargs="?")
    ap.add_argument("--window", help="FIRST:LAST host log lines to judge (LAST may be empty)")
    ap.add_argument("--map", help="Coop.map of the DLL that wrote the log")
    ap.add_argument("--json")
    ap.add_argument("--ru")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)
    host_path, comp_path = find_logs(a.log)
    if a.companion:
        comp_path = a.companion
    if not host_path or not os.path.exists(host_path):
        print("no host log in " + a.log, file=sys.stderr)
        return 4
    host = Log(host_path, "host")
    comp = Log(comp_path, "companion") if comp_path and os.path.exists(comp_path) else None
    window = None
    if a.window:
        first, _, last = a.window.partition(":")
        window = {host.path: (int(first or 1), int(last) if last else None)}
    ctx, findings = judge(host, comp, window, a.map)
    text = summary_ru(ctx, findings, "Анализ: " + host_path + (" + " + comp_path if comp else ""))
    if a.ru:
        with open(a.ru, "w", encoding="utf-8-sig") as f:
            f.write(text + "\n")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            f.write(results_json(ctx, findings))
    if not a.quiet:
        sys.stdout.reconfigure(encoding="utf-8")
        print(text)
    verdicts = {f.verdict for f in findings}
    if "CRASH" in verdicts:
        return 2
    if "FAIL" in verdicts:
        return 1
    if "INFRA" in verdicts:
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
