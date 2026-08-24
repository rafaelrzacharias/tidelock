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

    # Per COMMIT, not per range. A range-wide check let a later unrelated commit's [docs:none]
    # waive an earlier undocumented module change - and the gate's own fixture enshrined that as
    # correct behaviour until the fourth W0 review wrote the counter-example.
    # --no-merges: a merge commit's --name-only output is its conflict RESOLUTIONS - content
    # whose substantive commits each passed this gate individually on their branch. Requiring
    # [docs:none] on every wave-boundary merge would train people to paste it reflexively,
    # which is worse than the gap (the W1 platform PR measured exactly this: two merge commits
    # flagged, zero undocumented decisions in either). A merge that smuggles a NEW substantive
    # change in its resolution is a review problem, not a grep problem.
    commits = [c for c in run("git", "rev-list", "--reverse", "--no-merges", a.base + "..HEAD").splitlines() if c]
    missing = []
    for sha in commits:
        changed = [p for p in run("git", "show", "--name-only", "--format=", sha).splitlines() if p]
        message = run("git", "log", "-1", "--format=%B", sha)
        if "[docs:none]" in message:
            continue
        touched_docs = {p for p in changed if p.startswith("docs/")}
        for module, docs in MODULE_DOCS.items():
            prefix = "src/%s/" % module
            if any(p.startswith(prefix) for p in changed) and not touched_docs.intersection(docs):
                missing.append("%s in %s changed but none of %s did"
                               % (prefix, sha[:9], ", ".join(docs)))
    if not missing:
        print("commit_docs: %d commit(s) checked" % len(commits))

    for m in missing:
        print("ERROR commit_docs: " + m)
    if missing:
        print("       add the doc change, or put [docs:none] in the commit message")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
