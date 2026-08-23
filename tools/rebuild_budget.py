#!/usr/bin/env python3
"""Rebuild-time budget gate. Spec: docs/BUILD.md §3 (full < 10 s, incremental < 2 s, cold - no
compiler cache, ruling R-1), enforced as a PR gate by docs/TESTING.md §5.

Times a clean configure+build of the given preset, then touches one sim TU and times the
incremental build. Prints a TSV row for the CI artifact and exits non-zero on a breach.
"""
import argparse, os, shutil, subprocess, sys, time

import sys as _sys
_sys.stdout.reconfigure(encoding="utf-8", errors="replace")

FULL_BUDGET_S = 10.0
INCREMENTAL_BUDGET_S = 2.0


def run(cmd, cwd):
    t0 = time.perf_counter()
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        sys.exit("rebuild_budget: command failed: %s" % " ".join(cmd))
    return time.perf_counter() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", default="netcode-win")
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--touch", default="src/sim/sim.cpp")
    ap.add_argument("--strict-toolchain", choices=("on", "off"), default="on",
                    help="passed through to configure; CI runners carry stock clang, not the\n"
                         "pinned major (docs/BUILD.md §9 R-7)")
    ap.add_argument("--full-budget", type=float, default=FULL_BUDGET_S,
                    help="seconds; the default is the reference PC's (docs/BUILD.md §3). A CI box "
                         "has its own budget and must pass it explicitly rather than inherit ours.")
    ap.add_argument("--incremental-budget", type=float, default=INCREMENTAL_BUDGET_S)
    a = ap.parse_args()

    out = os.path.join(a.repo, "out", a.preset)
    if os.path.isdir(out):
        shutil.rmtree(out)                       # cold: the budget must hold without ccache

    run(["cmake", "--preset", a.preset,
         "-DTL_STRICT_TOOLCHAIN=" + a.strict_toolchain.upper()], a.repo)
    full = run(["cmake", "--build", "--preset", a.preset], a.repo)

    touched = os.path.join(a.repo, a.touch)
    if not os.path.exists(touched):
        sys.exit("rebuild_budget: --touch %s does not exist" % a.touch)
    os.utime(touched, None)
    incremental = run(["cmake", "--build", "--preset", a.preset], a.repo)

    print("preset\tfull_s\tfull_budget_s\tincremental_s\tincremental_budget_s")
    print("%s\t%.2f\t%.2f\t%.2f\t%.2f" % (a.preset, full, a.full_budget, incremental, a.incremental_budget))
    breach = []
    if full > a.full_budget:
        breach.append("full rebuild %.2fs > %.2fs" % (full, a.full_budget))
    if incremental > a.incremental_budget:
        breach.append("incremental %.2fs > %.2fs" % (incremental, a.incremental_budget))
    for b in breach:
        print("ERROR rebuild budget: " + b)
    return 1 if breach else 0


if __name__ == "__main__":
    sys.exit(main())
