#!/usr/bin/env python3
"""Doc-touch gate: a commit that changes src/<module>/ must change that module's doc, or say
[docs:none] in its message. Spec: CLAUDE.md doc-integrity protocol, docs/TESTING.md §5.

Docs say why; code says how. A module change with neither a doc change nor an explicit
[docs:none] is how the foundry program drifted.
"""
import argparse, subprocess, sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# src/<module>/ -> the docs that own it (docs/README.md "The docs" table).
MODULE_DOCS = {
    "foundation": ["docs/FX-PALETTE.md", "docs/MEMORY.md", "docs/CONTAINERS.md",
                   "docs/DETERMINISM.md", "docs/JOBS.md", "docs/CPP-SUBSET.md"],
    "core": ["docs/ECS.md", "docs/FRAME-LOOP.md", "docs/INPUT.md", "docs/ASSETS-AND-DATA.md"],
    "sim": ["docs/ALLOY.md"],
    "render": ["docs/RENDER2D.md"],
    "net": ["docs/NETCODE.md"],
    "platform": ["docs/PLATFORM.md"],
    "editor": ["docs/TOOLING.md"],
    "script": ["docs/LUAU-LAYER.md"],
    "app": ["docs/ARCHITECTURE.md"],
}


def run(*args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("commit_docs: %s failed:\n%s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="the commit the change is measured against")
    a = ap.parse_args()
    if not a.base or set(a.base) == {"0"}:
        print("commit_docs: no base commit to diff against - skipped")
        return 0
    probe = subprocess.run(["git", "rev-parse", "--verify", "--quiet", a.base + "^{commit}"],
                           capture_output=True, text=True)
    if probe.returncode != 0:
        # A force-push or a shallow clone can leave the base unreachable. Say so; do not crash,
        # and do not pretend the gate ran.
        print("commit_docs: base %s is not in this clone - skipped (not a pass)" % a.base)
        return 0

    changed = [p for p in run("git", "diff", "--name-only", a.base, "HEAD").splitlines() if p]
    message = run("git", "log", "--format=%B", a.base + "..HEAD")
    if "[docs:none]" in message:
        print("commit_docs: [docs:none] declared")
        return 0

    touched_docs = {p for p in changed if p.startswith("docs/")}
    missing = []
    for module, docs in MODULE_DOCS.items():
        prefix = "src/%s/" % module
        if any(p.startswith(prefix) for p in changed) and not touched_docs.intersection(docs):
            missing.append("%s changed but none of %s did" % (prefix, ", ".join(docs)))

    for m in missing:
        print("ERROR commit_docs: " + m)
    if missing:
        print("       add the doc change, or put [docs:none] in the commit message")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
