#!/usr/bin/env python3
"""The audits' own negative tests. Spec: docs/TESTING.md §5, docs/BUILD.md §10.3.

An audit is worth exactly what its negative test is worth. The W0 gates passed every obvious
planted violation and let five adversarial ones through; those five, and the three the fingerprint
review found, live here as fixtures so that every future change to the gates is re-tested instead
of trusted. A gate that stops firing fails this, loudly, in the PR lane.

Each case builds a throwaway tree under a temp dir - nothing is written into the repo - runs the
real audit against it, and asserts on the expected failure. The positive case (the repo itself is
clean) is the build's own audit targets, not this file.

Run: python tools/audit/selftest.py [--nm ...] [--objdump ...] [--ar ...] [--cxx ...]
"""
import argparse, json, os, shutil, subprocess, sys, tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
AUDIT = os.path.join(REPO, "tools", "audit")
results = []


def record(name, ok, detail=""):
    results.append((name, ok, detail))
    print("%-4s %s%s" % ("PASS" if ok else "FAIL", name, ("  - " + detail) if detail and not ok else ""))


def run(cmd, cwd=None):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def write(root, rel, text):
    path = os.path.join(root, rel.replace("/", os.sep))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w", encoding="utf-8", newline="\n").write(text)
    return path


def fixture_root(tmp, name):
    """A minimal src/ tree that includes.py accepts, with the REAL non-det stem list so the
    fixture cannot drift from src/foundation/CMakeLists.txt."""
    root = os.path.join(tmp, name)
    os.makedirs(os.path.join(root, "src", "foundation"), exist_ok=True)
    shutil.copyfile(os.path.join(REPO, "src", "foundation", "CMakeLists.txt"),
                    os.path.join(root, "src", "foundation", "CMakeLists.txt"))
    shutil.copyfile(os.path.join(REPO, "src", "foundation", "tl_types.h"),
                    os.path.join(root, "src", "foundation", "tl_types.h"))
    return root


# --- includes.py ------------------------------------------------------------------------------
# Each case: (name, relative path, source, a fragment the failure message must contain).
INCLUDE_CASES = [
    ("f32 alias in a sim TU", "src/sim/a.cpp",
     '#include "foundation/tl_types.h"\nextern const f32 x;\nconst f32 x = 1.0f;\n',
     "'f32' in a sim TU"),
    ("float in a sim TU whose basename collides with a non-det stem", "src/sim/fmt.cpp",
     '#include "foundation/tl_types.h"\nextern const float x;\nconst float x = 1.0f;\n',
     "'float' in a sim TU"),
    ("sim TU includes a peer module", "src/sim/b.cpp",
     '#include "net/wire.h"\n',
     "violates the module DAG"),
    ("sim TU includes a non-det foundation header", "src/sim/c.cpp",
     '#include "foundation/jobs.h"\n',
     "non-det foundation header"),
    ("banned system include", "src/sim/d.cpp",
     "#include <stdio.h>\n",
     "not on the allowlist"),
    ("static mutable state", "src/core/e.cpp",
     "static int counter = 0;\nint use(void) { return counter; }\n",
     "static mutable state"),
    ("thread_local outside the job system", "src/core/f.cpp",
     "thread_local int slot = 0;\n",
     "thread_local outside"),
    ("std:: in src/", "src/core/g.cpp",
     "void f(void) { std::abort(); }\n",
     "std:: in src/"),
    ("inline asm", "src/sim/h.cpp",
     "void f(void) { asm(\"nop\"); }\n",
     "inline asm"),
    ("rdtsc", "src/sim/i.cpp",
     "unsigned long long f(void) { return __rdtsc(); }\n",
     "rdtsc"),
    ("ISA intrinsic builtin", "src/sim/j.cpp",
     "int f(void) { return __builtin_ia32_pause(), 0; }\n",
     "ISA intrinsic"),
    ("header without a contract block", "src/core/k.h",
     "#pragma once\nvoid k_init(void);\n",
     "no contract block"),
    ("public function without a contract comment", "src/core/l.h",
     '#pragma once\n// Spec: docs/ECS.md §1\n#include "foundation/tl_types.h"\n'
     "void l_init(void);\n",
     "no contract comment"),
    ("the file contract block is not a function's contract comment", "src/core/l2.h",
     "#pragma once\n// Spec: docs/ECS.md §1\nvoid l2_init(void);\n",
     "no contract comment"),
    ("multi-line declaration without a contract comment", "src/core/m.h",
     '#pragma once\n// Spec: docs/ECS.md §1\n#include "foundation/tl_types.h"\n'
     "// documented\nvoid m_a(int x);\n"
     "void m_b(int x,\n        int y);\n",
     "no contract comment"),
    ("inline definition without a contract comment", "src/core/n.h",
     '#pragma once\n// Spec: docs/ECS.md §1\n#include "foundation/tl_types.h"\n'
     "int n_add(int a, int b) { return a + b; }\n",
     "no contract comment"),
    ("template definition without a contract comment", "src/core/o.h",
     '#pragma once\n// Spec: docs/ECS.md §1\n#include "foundation/tl_types.h"\n'
     "template <class T>\nT o_id(T x) { return x; }\n",
     "no contract comment"),
]

# Things that must NOT fire: the gates have to be usable, not just loud.
INCLUDE_CLEAN = [
    ("prose about floats in a sim header is not a violation", "src/sim/ok1.h",
     '#pragma once\n// Spec: docs/ALLOY.md §1\n// No float or double may appear below this line.\n'
     '#include "foundation/tl_types.h"\n\n// documented\nvoid ok1_step(void);\n'),
    ("a string literal mentioning double is not a violation", "src/sim/ok2.cpp",
     '#include "foundation/tl_types.h"\nextern const char* k;\nconst char* k = "double trouble";\n'),
    ("a commented-out include is not a violation", "src/sim/ok3.cpp",
     '// #include "net/wire.h"\n#include "foundation/tl_types.h"\n'),
    # fx.h will be an all-inline, all-template header: the shape the gate must handle without
    # drowning the lane in false positives.
    ("documented inline and template definitions pass", "src/sim/ok4.h",
     '#pragma once\n// Spec: docs/FX-PALETTE.md §10.1\n#include "foundation/tl_types.h"\n\n'
     "// Sum of two counts. Precondition: a + b does not overflow u32.\n"
     "inline u32 ok4_add(u32 a, u32 b) { return a + b; }\n\n"
     "// Widening multiply; the contract comment sits above the template head.\n"
     "template <class T>\n"
     "inline u64 ok4_mul(T a, T b) { return (u64)a * (u64)b; }\n"),
]


def test_includes(tmp):
    for name, rel, src, expect in INCLUDE_CASES:
        root = fixture_root(tmp, "inc_" + os.path.basename(rel).replace(".", "_"))
        write(root, rel, src)
        rc, out = run([sys.executable, os.path.join(AUDIT, "includes.py"), "--root", root])
        record("includes: " + name, rc == 1 and expect in out, out.strip()[:160])

    root = fixture_root(tmp, "inc_clean")
    for _name, rel, src in INCLUDE_CLEAN:
        write(root, rel, src)
    rc, out = run([sys.executable, os.path.join(AUDIT, "includes.py"), "--root", root])
    record("includes: no false positives on the clean fixtures", rc == 0, out.strip()[:400])


# --- symbols.py -------------------------------------------------------------------------------
LOWER_CPP = "extern const unsigned tl_lower;\nconst unsigned tl_lower = 1;\n"
UPPER_CPP = "extern const unsigned tl_upper;\nconst unsigned tl_upper = 2;\n"
UPWARD_CPP = ("extern const unsigned tl_upper;\n"
              "extern const unsigned tl_ref;\nconst unsigned tl_ref = tl_upper;\n")
MUTABLE_CPP = ("namespace { unsigned g_hidden = 0; }\n"
               "unsigned tl_bump(void) { g_hidden += 1u; return g_hidden; }\n")
LIBM_CPP = ('extern "C" double sqrt(double);\n'
            # Takes an argument on purpose: sqrt(2.0) is constant-folded and leaves no call to find.
            "double tl_m(double x) { return sqrt(x); }\n")


def build_lib(cxx, ar, root, name, sources):
    objs = []
    for i, src in enumerate(sources):
        cpp = write(root, "%s_%d.cpp" % (name, i), src)
        obj = cpp[:-4] + ".o"
        # -fno-builtin: the sim flag set (docs/CPP-SUBSET.md §7). Without it clang inlines libm
        # builtins and the symbol the audit is looking for never appears.
        rc, out = run([cxx, "-std=c++20", "-c", "-O1", "-fno-builtin", "-o", obj, cpp])
        if rc != 0:
            return None, out
        objs.append(obj)
    lib = os.path.join(root, "lib%s.a" % name)
    rc, out = run([ar, "rcs", lib] + objs)
    return (lib, "") if rc == 0 else (None, out)


def test_symbols(tmp, nm, objdump, ar, cxx):
    root = os.path.join(tmp, "sym")
    os.makedirs(root, exist_ok=True)
    allow = os.path.join(AUDIT, "allow.txt")

    lower, err = build_lib(cxx, ar, root, "lower", [LOWER_CPP, UPWARD_CPP])
    upper, err2 = build_lib(cxx, ar, root, "upper", [UPPER_CPP])
    if not lower or not upper:
        record("symbols: fixtures compile", False, (err or err2)[:200])
        return
    base = [sys.executable, os.path.join(AUDIT, "symbols.py"), "--nm", nm, "--objdump", objdump,
            "--allow", allow]
    rc, out = run(base + ["--layer", "lower=" + lower, "--layer", "upper=" + upper])
    record("symbols: a lower layer referencing an upper layer's symbol",
           rc == 1 and "undefined symbol outside the allowlist" in out, out.strip()[:200])

    mut, err = build_lib(cxx, ar, root, "mut", [MUTABLE_CPP])
    if mut:
        rc, out = run(base + ["--layer", "mut=" + mut])
        record("symbols: anonymous-namespace mutable global",
               rc == 1 and (".bss" in out or ".data" in out), out.strip()[:200])
    else:
        record("symbols: anonymous-namespace mutable global", False, err[:200])

    libm, err = build_lib(cxx, ar, root, "libm", [LIBM_CPP])
    if libm:
        rc, out = run(base + ["--layer", "libm=" + libm])
        record("symbols: a libm call from an audited lib",
               rc == 1 and "sqrt" in out, out.strip()[:200])
    else:
        record("symbols: a libm call from an audited lib", False, err[:200])

    clean, err = build_lib(cxx, ar, root, "clean", [LOWER_CPP])
    if clean:
        rc, out = run(base + ["--layer", "clean=" + clean])
        record("symbols: a clean lib passes", rc == 0, out.strip()[:200])
    else:
        record("symbols: a clean lib passes", False, err[:200])


# --- tier_parity.py ---------------------------------------------------------------------------
def cc_entry(directory, f, cmd):
    return {"directory": directory, "file": f, "command": cmd}


def test_tier_parity(tmp):
    n = os.path.join(tmp, "out", "netcode-x")
    s = os.path.join(tmp, "out", "ship-x")
    base = "clang++ -std=c++20 -O2 -Wall -c $REPO/src/sim/sim.cpp"
    write(n, "compile_commands.json",
          json.dumps([cc_entry(n, "sim.cpp", base + " -DTL_TIER_NETCODE=1")]))
    write(s, "compile_commands.json",
          json.dumps([cc_entry(s, "sim.cpp", base + " -DTL_TIER_SHIP=1 -DNDEBUG=1")]))
    rc, out = run([sys.executable, os.path.join(AUDIT, "tier_parity.py"), "--netcode", n, "--ship", s])
    record("tier_parity: only the stripping defines differ -> clean", rc == 0, out.strip()[:200])

    write(s, "compile_commands.json",
          json.dumps([cc_entry(s, "sim.cpp", base.replace("-O2", "-O1") + " -DTL_TIER_SHIP=1 -DNDEBUG=1")]))
    rc, out = run([sys.executable, os.path.join(AUDIT, "tier_parity.py"), "--netcode", n, "--ship", s])
    record("tier_parity: an optimisation delta is caught", rc == 1 and "-O1" in out, out.strip()[:200])


# --- fingerprint.py ---------------------------------------------------------------------------
WIN_CMD = ("clang-cl /nologo -TP -DWIN32 -D_WINDOWS -D_HAS_EXCEPTIONS=0 -DTL_TIER_NETCODE=1 "
           "-DTL_DEV=0 -DTL_SIM_TU=1 -std:c++20 /W4 /WX /EHs-c- /GR- /O2 /Z7 "
           "-I$REPO/src -c -o $BUILD/sim.obj $REPO/src/sim/sim.cpp")
NIX_CMD = ("clang++ -DTL_TIER_NETCODE=1 -DTL_DEV=0 -DTL_SIM_TU=1 -std=c++20 "
           "-Wall -Wextra -Werror -fno-exceptions -fno-rtti -O2 -gline-tables-only "
           "-I$REPO/src -c -o $BUILD/sim.o $REPO/src/sim/sim.cpp")


def fingerprint_id(tmp, tag, cmd, repo=REPO):
    out_dir = os.path.join(tmp, "fp_" + tag)
    db = write(out_dir, "compile_commands.json",
               json.dumps([cc_entry(out_dir, repo.replace("\\", "/") + "/src/sim/sim.cpp",
                                    cmd.replace("$REPO", repo.replace("\\", "/"))
                                       .replace("$BUILD", out_dir.replace("\\", "/")))]))
    rc, out = run([sys.executable, os.path.join(REPO, "tools", "fingerprint.py"),
                   "--repo", repo, "--tier", "netcode", "--compiler", "clang " + tag,
                   "--compile-commands", db, "--binary-dir", out_dir,
                   "--out-cpp", os.path.join(out_dir, "build_id.cpp"), "--print"])
    return (out.strip().splitlines() or [""])[-1] if rc == 0 else "FAILED: " + out


def test_fingerprint(tmp):
    win = fingerprint_id(tmp, "win", WIN_CMD)
    nix = fingerprint_id(tmp, "nix", NIX_CMD)
    record("fingerprint: build_id is target-independent (docs/BUILD.md §9 R-8)",
           win == nix and not win.startswith("FAILED"), "win=%s nix=%s" % (win[:16], nix[:16]))

    define = fingerprint_id(tmp, "define", NIX_CMD + " -DTL_EXTRA=1")
    record("fingerprint: a new define changes build_id", define != nix, define[:32])

    platform_define = fingerprint_id(tmp, "platdef", NIX_CMD + " -DUNICODE")
    record("fingerprint: a platform define does not change build_id", platform_define == nix,
           platform_define[:32])

    std = fingerprint_id(tmp, "std", NIX_CMD.replace("-std=c++20", "-std=c++17"))
    record("fingerprint: the language standard changes build_id", std != nix, std[:32])

    same = fingerprint_id(tmp, "same", NIX_CMD)
    record("fingerprint: identical inputs are stable", same == nix, same[:32])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", default="llvm-nm")
    ap.add_argument("--objdump", default="llvm-objdump")
    ap.add_argument("--ar", default="llvm-ar")
    ap.add_argument("--cxx", default="clang++")
    a = ap.parse_args()

    for tool in (a.nm, a.objdump, a.ar, a.cxx):
        if not shutil.which(tool):
            sys.exit("selftest: %s is not on PATH - the gates cannot be tested without it" % tool)

    with tempfile.TemporaryDirectory(prefix="tl_selftest_") as tmp:
        test_includes(tmp)
        test_symbols(tmp, a.nm, a.objdump, a.ar, a.cxx)
        test_tier_parity(tmp)
        test_fingerprint(tmp)

    failed = [r for r in results if not r[1]]
    print("selftest: %d checks, %d failed" % (len(results), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
