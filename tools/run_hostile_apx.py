#!/usr/bin/env python3
"""Run the hostile `.apx` cases and report which ones break the player.

    tools/make_hostile_apx.py --out work/hostile/cases
    tools/run_hostile_apx.py  work/hostile/cases

Each case is run in its own process with a watchdog, because a hostile project
is exactly as likely to hang the loader as it is to crash it, and a hang in
`Project.cpp` happens before the player's own `--frame-timeout` watchdog exists.

Rows are classified as
    ok        exited 0 within the budget
    crash     died on a signal (the signal number is shown)
    timeout   the watchdog had to kill it
    errors    exited 0 but the log contains error-severity lines

Writes REPORT.md next to the logs so a run can be reviewed later.
"""

import argparse
import collections
import os
import re
import signal
import subprocess
import sys
import time

DEFAULT_PLAYER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                              "build", "neo_apex_player")

# Log lines that are error-severity and worth surfacing even when the process
# exits cleanly.  `(Err)` is the engine's own logger.
ERROR_PATTERNS = [
    ("phxvk error", re.compile(r"\[phxvk error\] (.*)")),
    ("engine Err", re.compile(r"\(Err\)\s+(.*)")),
    ("assert", re.compile(r"(Assertion|assert).*")),
]

# Lines that show what the shim decided about a shader, which is the interesting
# outcome for most of the cases even when nothing crashes.
NOTE_PATTERNS = [
    ("gs emulated", re.compile(r"geometry shader emulated: (.*)")),
    ("gs refused", re.compile(r"geometry shader not emulated.*?: (.*)")),
    ("gs dropped", re.compile(r"geometry shader stage DROPPED for pipeline #(\d+)")),
    ("varying widened", re.compile(r"vertex/pixel interface disagreement[^:]*: (.*)")),
    ("widening refused", re.compile(r"could not be widened to four components \((.*)")),
    ("pipeline fail", re.compile(r"vkCreateGraphicsPipelines failed.*")),
    ("no rt", re.compile(r"no render targets were created.*")),
]


def one_case(player, path, frames, budget):
    log = []
    started = time.time()
    # --headless: do not show or focus a window.  Without it every case puts a
    # window in front of whatever the person at the keyboard is doing.
    # SDL_MAC_BACKGROUND_APP=1 is the one that matters on macOS: SDL otherwise
    # brings the process to the foreground when it launches, which a sweep does
    # once per case.  The flag sets it too, but exporting it here means it is in
    # place before the process starts.
    env = dict(os.environ, NEO_APEX_HEADLESS="1", SDL_MAC_BACKGROUND_APP="1")
    proc = subprocess.Popen([player, path, "--headless", "--no-ui", "--no-audio", "--no-vsync",
                             "--fixed-fps", "--frames", str(frames)], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace", start_new_session=True)
    try:
        out, _ = proc.communicate(timeout=budget)
        log = out.splitlines()
        status = proc.returncode
    except subprocess.TimeoutExpired:
        # start_new_session put it in its own process group, so this takes the
        # whole thing down rather than just the parent.
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        out, _ = proc.communicate()
        log = out.splitlines()
        status = None
    elapsed = time.time() - started

    errors = []
    for _, pattern in ERROR_PATTERNS:
        for line in log:
            m = pattern.search(line)
            if m:
                errors.append(m.group(1).strip()[:160])

    notes = collections.OrderedDict()
    for label, pattern in NOTE_PATTERNS:
        for line in log:
            m = pattern.search(line)
            if m:
                key = label
                if key not in notes:
                    notes[key] = m.group(1).strip()[:120] if m.groups() else ""
                break

    if status is None:
        verdict = "timeout"
    elif status < 0:
        verdict = "crash(%s)" % signal.Signals(-status).name
    elif status != 0:
        verdict = "exit(%d)" % status
    elif errors:
        verdict = "errors(%d)" % len(errors)
    else:
        verdict = "ok"

    return dict(path=path, verdict=verdict, status=status, elapsed=elapsed,
                errors=errors, notes=notes, log=log)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", help="directory of .apx cases")
    ap.add_argument("--player", default=DEFAULT_PLAYER)
    ap.add_argument("--frames", type=int, default=240)
    ap.add_argument("--timeout", type=float, default=90.0, help="seconds per case")
    ap.add_argument("--only", default="")
    ap.add_argument("--logs", default="", help="directory for per-case logs")
    args = ap.parse_args()

    if not os.path.exists(args.player):
        raise SystemExit("no player at %s" % args.player)
    cases = sorted(f for f in os.listdir(args.dir) if f.endswith(".apx"))
    if args.only:
        cases = [c for c in cases if args.only in c]
    if not cases:
        raise SystemExit("no .apx files in %s" % args.dir)
    if args.logs:
        os.makedirs(args.logs, exist_ok=True)

    print("%-34s %-12s %7s  %s" % ("case", "verdict", "sec", "note"))
    print("-" * 100)
    results = []
    for name in cases:
        path = os.path.join(args.dir, name)
        r = one_case(args.player, path, args.frames, args.timeout)
        r["name"] = name[:-4]
        results.append(r)
        if args.logs:
            with open(os.path.join(args.logs, name[:-4] + ".log"), "w") as f:
                f.write("\n".join(r["log"]) + "\n")
        note = "; ".join("%s=%s" % (k, v) if v else k for k, v in r["notes"].items())
        if not note and r["errors"]:
            note = r["errors"][0]
        print("%-34s %-12s %7.1f  %s" % (r["name"], r["verdict"], r["elapsed"], note[:70]))

    # The interesting ones, in full.
    bad = [r for r in results if r["verdict"] != "ok"]
    print()
    print("=" * 100)
    print("%d case(s), %d not clean" % (len(results), len(bad)))
    for r in bad:
        print()
        print("### %s  ->  %s  (%.1fs)" % (r["name"], r["verdict"], r["elapsed"]))
        for e in r["errors"][:6]:
            print("    err: %s" % e)
        for k, v in r["notes"].items():
            print("    %s: %s" % (k, v))
        if args.logs:
            print("    log: %s" % os.path.join(args.logs, r["name"] + ".log"))

    manifest = os.path.join(args.dir, "CASES.md")
    with open(os.path.join(args.dir, "REPORT.md"), "w") as f:
        f.write("# Hostile .apx run\n\n")
        if os.path.exists(manifest):
            f.write("Cases are described in CASES.md.\n\n")
        f.write("| case | verdict | seconds | notes |\n| --- | --- | --- | --- |\n")
        for r in results:
            note = "; ".join("%s=%s" % (k, v) if v else k for k, v in r["notes"].items())
            if not note and r["errors"]:
                note = r["errors"][0]
            f.write("| `%s` | %s | %.1f | %s |\n"
                    % (r["name"], r["verdict"], r["elapsed"],
                       note.replace("|", "\\|")[:120]))
        f.write("\n## Failures in full\n")
        for r in bad:
            f.write("\n### %s -> %s\n\n```\n" % (r["name"], r["verdict"]))
            f.write("\n".join(r["errors"][:10]))
            f.write("\n```\n")
    print("\nwrote %s" % os.path.join(args.dir, "REPORT.md"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
