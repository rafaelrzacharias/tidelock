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
    # The W1 fx review's plants: a float or a long with NO banned type token anywhere.
    ("a double through decltype of a literal", "src/sim/a2.cpp",
     '#include "foundation/tl_types.h"\nu32 a2(u32 k) { decltype(1.0) x = 1.5; x *= k; return (u32)x; }\n',
     "floating literal '1.0'"),
    ("a hex float literal", "src/sim/a3.cpp",
     '#include "foundation/tl_types.h"\nu32 a3(u32 k) { auto y = 0x1p3; return (u32)(k * y); }\n',
     "floating literal '0x1p3'"),
    ("an exponent literal promoting an integer expression", "src/sim/a4.cpp",
     '#include "foundation/tl_types.h"\nu32 a4(u32 k) { return (u32)(k * 1e3); }\n',
     "floating literal '1e3'"),
    ("_Float16 is a float type", "src/sim/a5.cpp",
     '#include "foundation/tl_types.h"\nu32 a5(u32 k) { _Float16 h = (_Float16)k; return (u32)h; }\n',
     "'_Float16' in a sim TU"),
    ("a long through the L suffix", "src/sim/a6.cpp",
     '#include "foundation/tl_types.h"\nu64 a6(u64 k) { decltype(1L) x = (decltype(1L))k; return (u64)(x * 3); }\n',
     "integer literal '1L'"),
    ("an unsigned long through the lu suffix", "src/sim/a7.cpp",
     '#include "foundation/tl_types.h"\nu64 a7(u64 k) { return k + 0x10lu; }\n',
     "integer literal '0x10lu'"),
    ("float in a sim TU whose basename collides with a non-det stem", "src/sim/fmt.cpp",
     '#include "foundation/tl_types.h"\nextern const float x;\nconst float x = 1.0f;\n',
     "'float' in a sim TU"),
    ("sim TU includes a peer module", "src/sim/b.cpp",
     '#include "net/wire.h"\n',
     "violates the module DAG"),
    ("sim TU includes a non-det foundation header", "src/sim/c.cpp",
     '#include "foundation/jobs.h"\n',
     "non-det foundation header"),
    # The panic-ABI exemption is the HEADER only (docs/CPP-SUBSET.md §9 R-3): its runtime and its
    # non-det siblings stay barred.
    ("sim TU includes the panic runtime, not just its header", "src/sim/c2.cpp",
     '#include "foundation/tl_assert.cpp"\n',
     "non-det foundation header"),
    ("sim TU includes tl_log.h (non-det, not the panic ABI)", "src/sim/c3.cpp",
     '#include "foundation/tl_log.h"\n',
     "non-det foundation header"),
    # RR-7 (docs/CPP-SUBSET.md §1): the tooling io/state exemption is keyed by STEM, not directory -
    # a non-det stem that is not on TL_FOUNDATION_TOOLING inherits nothing from its neighbours.
    ("RR-7: a non-tooling non-det stem gets no io allowance", "src/foundation/jobs.cpp",
     "#include <stdio.h>\n",
     "not on the allowlist"),
    ("RR-7: a non-tooling non-det stem gets no static-state allowance", "src/foundation/mem_pool.cpp",
     "static int g_x = 0;\nint use(void) { return g_x; }\n",
     "static mutable state"),
    # RR-7, the other direction: the exemption is stem-keyed and tl_assert's stem is ON the list -
    # but tl_assert.h is also the ONE tooling header a sim TU may include (PANIC_ABI_HEADER), so
    # granting the HEADER io/state pushes both straight through R-3's hole into every det TU in
    # the tree. Both of these reported 0 violations before includes.py excluded it by path.
    ("RR-7: the panic-ABI HEADER gets no io allowance", "src/foundation/tl_assert.h",
     "#include <stdio.h>\n",
     "not on the allowlist"),
    ("RR-7: the panic-ABI HEADER gets no static-state allowance", "src/foundation/tl_assert.h",
     "static int g_ta = 0;\nint ta_use(void) { return g_ta; }\n",
     "static mutable state"),
    # R-3 and R-4 read together: tl_assert.h is the ONLY tooling header a sim TU may include. One
    # fixture per barred header - "tl_log.h already covers that case" is exactly how the __GNUC__
    # fixture came to never exercise the win leg (LESSONS.md).
    ("sim TU includes tl_prof.h (non-det, not the panic ABI)", "src/sim/c4.cpp",
     '#include "foundation/tl_prof.h"\n',
     "non-det foundation header"),
    ("sim TU includes tl_probe.h (non-det, not the panic ABI)", "src/sim/c5.cpp",
     '#include "foundation/tl_probe.h"\n',
     "non-det foundation header"),
    ("sim TU includes crash.h (non-det, not the panic ABI)", "src/sim/c6.cpp",
     '#include "foundation/crash.h"\n',
     "non-det foundation header"),
    ("banned system include", "src/sim/d.cpp",
     "#include <stdio.h>\n",
     "not on the allowlist"),
    ("static mutable state", "src/core/e.cpp",
     "static int counter = 0;\nint use(void) { return counter; }\n",
     "static mutable state"),
    ("thread_local outside the job system", "src/core/f.cpp",
     "thread_local int slot = 0;\n",
     "thread-local storage outside"),
    ("__thread in a sim TU", "src/sim/tls1.cpp",
     "__thread int slot;\n",
     "thread-local storage outside"),
    ("__declspec(thread) in a sim TU", "src/sim/tls2.cpp",
     "__declspec(thread) int slot;\n",
     "thread-local storage outside"),
    ("long in a sim TU - 32-bit on Windows, 64-bit on Linux", "src/sim/abi1.cpp",
     '#include "foundation/tl_types.h"\nlong abi1(void) { return 1; }\n',
     "'long' in a sim TU"),
    ("char in a sim TU - signed on x86-64, unsigned on aarch64", "src/sim/abi2.cpp",
     '#include "foundation/tl_types.h"\nint abi2(char c) { return (int)c; }\n',
     "'char' in a sim TU"),
    ("a digit separator must not blind the rest of the file", "src/sim/sep.cpp",
     '#include "foundation/tl_types.h"\nu32 a(void) { return 1\'000u; }\n'
     "extern const f32 leaked;\nconst f32 leaked = 1.0f;\n",
     "'f32' in a sim TU"),
    ("bit-field in a sim TU - layout differs windows-msvc vs linux/aarch64", "src/sim/bf.cpp",
     '#include "foundation/tl_types.h"\nstruct Row { u8 a : 4; u16 c : 8; };\n',
     "bit-field in a sim TU"),
    ("platform macro in a sim TU", "src/sim/plat.cpp",
     '#include "foundation/tl_types.h"\n#if defined(_WIN32)\nu32 k(void) { return 1u; }\n#endif\n',
     "'_WIN32' in a sim TU"),
    # A comment is not char data - the doc-citation style is full of section signs - so the rule
    # is about literals, and so is the fixture.
    ("non-ASCII byte in a sim-TU string literal", "src/sim/utf8.cpp",
     '#include "foundation/tl_types.h"\n'
     'extern const char* k;\nconst char* k = "café";\n',
     "non-ASCII byte in a sim-TU literal"),
    ("enum without a fixed underlying type in a sim TU", "src/sim/en.cpp",
     '#include "foundation/tl_types.h"\nenum Phase { PHASE_A, PHASE_B };\n',
     "fixed underlying type"),
    ("custom section hides a global from the .data gate", "src/sim/sec.cpp",
     '#include "foundation/tl_types.h"\n__attribute__((section(".tl_hidden"))) u32 g_x;\n',
     "custom section attribute"),
    ("undocumented operator in a header", "src/sim/op.h",
     '#pragma once\n// Spec: docs/ALLOY.md §1\n#include "foundation/tl_types.h"\n\n'
     "struct V { u32 x; };\n"
     "inline V operator+(V a, V b) { return V{ a.x + b.x }; }\n",
     "no contract comment"),
    ("undocumented __attribute__-prefixed function", "src/sim/attr.h",
     '#pragma once\n// Spec: docs/ALLOY.md §1\n#include "foundation/tl_types.h"\n\n'
     "__attribute__((always_inline)) inline u32 attr_f(u32 a) { return a; }\n",
     "no contract comment"),
    ("a '(' inside a string literal must not swallow later declarations", "src/sim/paren.h",
     '#pragma once\n// Spec: docs/ALLOY.md §1\n#include "foundation/tl_types.h"\n\n'
     "// Reports a parse failure. Never returns.\n"
     'inline void bad(void) { fail("expected ("); }\n'
     "u32 hidden_one(u32 a);\n",
     "no contract comment"),
    ("a function under a one-line template definition needs its own comment", "src/sim/tdef.h",
     '#pragma once\n// Spec: docs/ALLOY.md §1\n#include "foundation/tl_types.h"\n\n'
     "// Identity. Returns its argument unchanged.\n"
     "template <class T> T tdef_id(T x) { return x; }\n"
     "u32 tdef_hidden(u32 a);\n",
     "no contract comment"),
    # The fourth review's self-attack: a hex escape is a non-ASCII byte the source-character scan
    # cannot see, and `size_t` is the identity hazard `usize` was fixed for.
    ("a high hex escape in a sim-TU literal", "src/sim/esc.cpp",
     '#include "foundation/tl_types.h"\n'
     'extern const char* k;\nconst char* k = "' + chr(92) + 'xE9";\n',
     "non-ASCII byte in a sim-TU literal"),
    ("size_t in a sim TU", "src/sim/szt.cpp",
     '#include "foundation/tl_types.h"\nu32 f(size_t n) { return (u32)n; }\n',
     "'size_t' in a sim TU"),
    ("compile-time wall clock", "src/sim/clk.cpp",
     '#include "foundation/tl_types.h"\n'
     'extern const char* b;\nconst char* b = __DATE__;\n',
     "compile-time wall clock"),
    # The fifth review: the boundary of the cross-target gate's no-sysroot model, which only a
    # token ban can cover.
    ("int_fast16_t in a sim TU", "src/sim/fastint.cpp",
     '#include "foundation/tl_types.h"\nu32 f(int_fast16_t n) { return (u32)n; }\n',
     "'int_fast16_t' in a sim TU"),
    ("__has_include in a sim TU", "src/sim/hasinc.cpp",
     '#include "foundation/tl_types.h"\n#if __has_include(<vcruntime.h>)\nu32 g(void) { return 1u; }\n#endif\n',
     "__has_include in a sim TU"),
    ("__LP64__ in a sim TU", "src/sim/lp64.cpp",
     '#include "foundation/tl_types.h"\n#if __LP64__\nu32 s(void) { return 1u; }\n#endif\n',
     "'__LP64__' in a sim TU"),
    ("a sim TU includes the float bridge", "src/sim/bridge.cpp",
     '#include "foundation/fx_float.h"\n',
     "includes the float bridge"),
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
     '#include "foundation/tl_types.h"\n\n'
     "// Advances the fixture one step. No preconditions.\nvoid ok1_step(void);\n"),
    ("a string literal mentioning double is not a violation", "src/sim/ok2.cpp",
     '#include "foundation/tl_types.h"\nextern const char* k;\nconst char* k = "double trouble";\n'),
    ("LL/ULL literals, member access after a digit, hex integers and a 1.5 in a comment are clean", "src/sim/ok9.cpp",
     '#include "foundation/tl_types.h"\n// 1.5 * 2^23 is the wrong magic (see fx_float.h)\n'
     'struct V { i32 v; };\nu64 ok9(u64 k) { V a[2] = { { 1 }, { 2 } }; const i64 m = 1LL << 40; '
     "return k + 0x1fULL + 1'000ull + (u64)a[1].v + (u64)m + 7u; }\n"),
    # docs/CPP-SUBSET.md §9 R-3: the first TL_CHECK in fx.h includes tl_assert.h from a det TU.
    ("a sim TU may include the panic ABI header tl_assert.h", "src/sim/ok8.cpp",
     '#include "foundation/tl_assert.h"\nu32 ok8(u32 a) { TL_CHECK(a != 0u); return a; }\n'),
    ("a commented-out include is not a violation", "src/sim/ok3.cpp",
     '// #include "net/wire.h"\n#include "foundation/tl_types.h"\n'),
    ("internal-linkage functions and constants are not mutable state", "src/sim/ok5.cpp",
     '#include "foundation/tl_types.h"\n'
     "static u32 helper(void) { return 1u; }\n"
     "static u32 const K = 2u;\n"
     "static constexpr u32 C = 3u;\n"
     "u32 use(void) { return helper() + K + C; }\n"),
    ("a message literal keeps const char*", "src/sim/ok6.cpp",
     '#include "foundation/tl_types.h"\n'
     "void fail(const char* msg);\nvoid f(void) { fail(\"bad\"); }\n"),
    # The shapes fx.h will actually be written in. Every one of these was a false positive that
    # would have stalled the critical-path lane on its first commit.
    ("fx.h idioms do not false-positive", "src/sim/ok7.h",
     '#pragma once\n// Spec: docs/FX-PALETTE.md §10.1\n#include "foundation/tl_types.h"\n\n'
     "// Widening multiply of two palette rows. Precondition: FRAC_A + FRAC_B <= 62.\n"
     "template <class A,\n          class B>\n"
     "inline u64 ok7_mul(A a, B b) { return (u64)a * (u64)b; }\n\n"
     "// Saturating add. Returns the clamped sum; never traps.\n"
     "[[nodiscard]]\n"
     "inline u32 ok7_sat(u32 a, u32 b) { return a + b; }\n\n"
     "/* Divides two rows.\n"
     " * Precondition: b is non-zero; division by zero is a caller bug.\n"
     " */\n"
     "inline u32 ok7_div(u32 a, u32 b) { return a / b; }\n\n"
     "// Equality over the raw representation. Exact, never approximate.\n"
     "inline bool operator==(u32 a, i32 b) { return a == (u32)b; }\n\n"
     "// Sign of a value. Returns -1, 0 or 1 - a ternary is not a bit-field.\n"
     "inline i32 ok7_sign(i32 x) { return x < 0 ? -1 : (x > 0 ? 1 : 0); }\n\n"
     "// Compiled only in dev tiers; same contract either way.\n"
     "#if TL_DEV\n"
     "inline u32 ok7_probe(u32 a) { return a; }\n"
     "#endif\n"),
    # fx.h will be an all-inline, all-template header: the shape the gate must handle without
    # drowning the lane in false positives.
    ("documented inline and template definitions pass", "src/sim/ok4.h",
     '#pragma once\n// Spec: docs/FX-PALETTE.md §10.1\n#include "foundation/tl_types.h"\n\n'
     "// Sum of two counts. Precondition: a + b does not overflow u32.\n"
     "inline u32 ok4_add(u32 a, u32 b) { return a + b; }\n\n"
     "// Widening multiply; the contract comment sits above the template head.\n"
     "template <class T>\n"
     "inline u64 ok4_mul(T a, T b) { return (u64)a * (u64)b; }\n"),
    # RR-7: the positive half - a stem named on TL_FOUNDATION_TOOLING gets real io and mutable
    # state. Without this fixture the negative ones above could pass for the wrong reason (the
    # whole gate silently broken) instead of the right one (the exemption is stem-keyed).
    ("RR-7: a tooling stem gets real io and mutable state", "src/foundation/log.cpp",
     "#include <stdio.h>\n#include <stdlib.h>\n"
     "namespace { unsigned g_ring_head = 0; }\n"
     "void tl_log_bump(void) { g_ring_head += 1u; }\n"),
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
WEAK_CPP = ('extern "C" __attribute__((weak)) double sqrt(double);\n'
            "double tl_w(double x) { return sqrt(x); }\n")
RELRO_CPP = ('extern "C" int tl_a(void) { return 1; }\nextern "C" int tl_b(void) { return 2; }\n'
             "using Fn = int (*)(void);\nextern const Fn table[2];\n"
             "const Fn table[2] = { tl_a, tl_b };\n")
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


def build_lib_named(cxx, ar, root, libname, filename, src):
    """Like build_lib, but the member keeps an EXACT source filename - RR-7's exemption is keyed
    by the archive member's stem, so the fixture must control it precisely."""
    cpp = write(root, filename, src)
    obj = cpp[:-4] + ".o"
    rc, out = run([cxx, "-std=c++20", "-c", "-O1", "-fno-builtin", "-o", obj, cpp])
    if rc != 0:
        return None, out
    lib = os.path.join(root, "lib%s.a" % libname)
    rc, out = run([ar, "rcs", lib, obj])
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

    # Weak undefined symbols are an ELF concept; the same source on COFF produces no `w` entry
    # (and drags in _fltused), so this one is built for ELF explicitly.
    weak_obj = os.path.join(root, "weak.o")
    weak_src = write(root, "weak.cpp", WEAK_CPP)
    rc, out = run([cxx, "--target=x86_64-linux-gnu", "-ffreestanding", "-nostdinc", "-fno-builtin",
                   "-std=c++20", "-c", "-o", weak_obj, weak_src])
    if rc == 0:
        weak_lib = os.path.join(root, "libweak.a")
        run([ar, "rcs", weak_lib, weak_obj])
        rc, out = run(base + ["--layer", "weak=" + weak_lib])
        record("symbols: a weak undefined libm symbol", rc == 1 and "sqrt" in out, out.strip()[:200])
    else:
        record("symbols: a weak undefined libm symbol", False, out[:200])

    # PIE is the default on Linux and the Pi: a const function-pointer table lands in
    # .data.rel.ro, which is read-only after relocation and must not read as mutable state.
    relro_obj = os.path.join(root, "relro.o")
    relro_src = write(root, "relro.cpp", RELRO_CPP)
    rc, out = run([cxx, "--target=aarch64-linux-gnu", "-ffreestanding", "-nostdinc", "-fPIE",
                   "-std=c++20", "-c", "-o", relro_obj, relro_src])
    if rc == 0:
        relro_lib = os.path.join(root, "librelro.a")
        run([ar, "rcs", relro_lib, relro_obj])
        rc, out = run(base + ["--layer", "relro=" + relro_lib])
        record("symbols: .data.rel.ro under PIE is not mutable state", rc == 0, out.strip()[:200])
    else:
        record("symbols: .data.rel.ro under PIE is not mutable state", False, out[:200])

    clean, err = build_lib(cxx, ar, root, "clean", [LOWER_CPP])
    if clean:
        rc, out = run(base + ["--layer", "clean=" + clean])
        record("symbols: a clean lib passes", rc == 0, out.strip()[:200])
    else:
        record("symbols: a clean lib passes", False, err[:200])


# --- RR-7: the tooling-plane writable-static exemption (docs/CPP-SUBSET.md §1) ----------------
def test_symbols_tooling(tmp, nm, objdump, ar, cxx):
    root = os.path.join(tmp, "sym_rr7")
    os.makedirs(root, exist_ok=True)
    # fixture_root() copies the REAL src/foundation/CMakeLists.txt, so this reads the actual
    # TL_FOUNDATION_TOOLING line - the fixture cannot drift from what ships.
    fixture = fixture_root(tmp, "sym_rr7_root")
    allow = os.path.join(AUDIT, "allow.txt")
    base = [sys.executable, os.path.join(AUDIT, "symbols.py"), "--nm", nm, "--objdump", objdump,
            "--allow", allow]

    tool_lib, err = build_lib_named(cxx, ar, root, "tool", "log.cpp", MUTABLE_CPP)
    nontool_lib, err2 = build_lib_named(cxx, ar, root, "nontool", "jobs.cpp", MUTABLE_CPP)
    clean_lib, err3 = build_lib(cxx, ar, root, "clean", [LOWER_CPP])   # --layer is mandatory
    if not tool_lib or not nontool_lib or not clean_lib:
        record("symbols: RR-7 fixtures compile", False, (err or err2 or err3)[:200])
        return
    layer = ["--layer", "clean=" + clean_lib]

    rc, out = run(base + layer + ["--root", fixture, "--tooling-lib", "tool",
                                  "--data-only", "tool=" + tool_lib])
    record("symbols: RR-7 exempts a TL_FOUNDATION_TOOLING stem (log)", rc == 0, out.strip()[:200])

    rc, out = run(base + layer + ["--root", fixture, "--tooling-lib", "nontool",
                                  "--data-only", "nontool=" + nontool_lib])
    record("symbols: RR-7 does not exempt a non-tooling non-det stem (jobs)",
           rc == 1 and (".bss" in out or ".data" in out), out.strip()[:200])

    rc, out = run(base + layer + ["--data-only", "tool=" + tool_lib])   # no --root at all
    record("symbols: RR-7's exemption is opt-in - no --root means no exemption",
           rc == 1 and (".bss" in out or ".data" in out), out.strip()[:200])

    # RR-7 is a LIB + STEM exemption, never a stem alone. `log`/`prof`/`probe`/`crash` are ordinary
    # words: keying on the archive member's stem alone exempted a log.o in ANY --data-only lib -
    # i.e. every non-audited lib in src/. Measured before --tooling-lib existed: this exact object
    # under the name tl_platform reported 0 violations.
    rc, out = run(base + layer + ["--root", fixture, "--tooling-lib", "tl_foundation",
                                  "--data-only", "tl_platform=" + tool_lib])
    record("symbols: RR-7 does NOT exempt the same log.o in another lib",
           rc == 1 and (".bss" in out or ".data" in out), out.strip()[:200])

    rc, out = run(base + layer + ["--root", fixture, "--data-only", "tool=" + tool_lib])
    record("symbols: --root without --tooling-lib grants no exemption",
           rc == 1 and (".bss" in out or ".data" in out), out.strip()[:200])


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

    for flag in ("-include evil.h", "-UTL_DEV", "-funsigned-char", "-fshort-enums",
                 "-fpack-struct=1"):
        tag = flag.split()[0].strip("-=").replace("-", "_")
        got = fingerprint_id(tmp, "flag_" + tag, NIX_CMD + " " + flag)
        record("fingerprint: %s changes build_id" % flag, got != nix, got[:32])

    # The two shapes that made build-id-cross-target red in CI while passing locally.
    quoted = fingerprint_id(tmp, "quoted", '"C:/Program Files/LLVM/bin/clang-cl.exe" '
                            + WIN_CMD.split(" ", 1)[1])
    plain = fingerprint_id(tmp, "plain", WIN_CMD)
    record("fingerprint: a quoted compiler path does not change build_id", quoted == plain,
           "quoted=%s plain=%s" % (quoted[:16], plain[:16]))

    isys = fingerprint_id(tmp, "isys", NIX_CMD + " -isystem $REPO/vendor")
    imsvc = fingerprint_id(tmp, "imsvc", WIN_CMD + " -imsvc$REPO/vendor")
    record("fingerprint: -isystem <dir> and -imsvc<dir> agree", isys == imsvc,
           "isystem=%s imsvc=%s" % (isys[:16], imsvc[:16]))

    for flag in ("-ffp-model=fast", "-ffp-contract=fast"):
        got = fingerprint_id(tmp, "fm" + flag[-4:], NIX_CMD + " " + flag)
        record("fingerprint: %s is refused, not recorded" % flag,
               got.startswith("FAILED") and "banned" in got, got[:140])

    # A tree with no git history can see no source change at all; that must be an error, not a
    # constant id (docs/BUILD.md §5).
    nogit = os.path.join(tmp, "nogit", "src", "sim")
    os.makedirs(nogit, exist_ok=True)
    write(os.path.join(tmp, "nogit"), "src/sim/sim.cpp", "int f(void) { return 1; }\n")
    rc, out = run([sys.executable, os.path.join(REPO, "tools", "fingerprint.py"),
                   "--repo", os.path.join(tmp, "nogit"), "--tier", "netcode",
                   "--out-cpp", os.path.join(tmp, "nogit", "build_id.cpp")])
    record("fingerprint: a tree with no git history is refused, not fingerprinted blind",
           rc != 0 and "not a git repository" in out, out.strip()[:160])


# --- targets.py -------------------------------------------------------------------------------
# The constructs four reviews found and no regex caught. Each is measured on all three triples,
# so these fixtures are the evidence that measurement beats enumeration - and the clean ones are
# the evidence it does not cry wolf on ordinary sim code.
TARGET_CASES = [
    ("[[no_unique_address]] changes sizeof", "nua",
     "struct E {};\nstruct V { [[no_unique_address]] E e; u32 x; };\n", "different layout"),
    ("#pragma pack + alignas changes layout", "pack",
     "#pragma pack(1)\nstruct P { u8 a; alignas(8) u64 b; };\n#pragma pack()\n", "different layout"),
    ("a bit-field changes layout", "bits",
     "struct R { u8 a : 4; u16 c : 8; };\n", "different layout"),
    ("a compiler-macro branch is two programs", "gnuc",
     "#ifdef __GNUC__\nu32 k(void) { return 1u; }\n#else\nu32 k(void) { return 2u; }\n#endif\n",
     "preprocessed source differs"),
    # The W1 rng/hash review's finding. Vendoring rapidhash put `-U_MSC_VER` in BASE_FLAGS to
    # silence clang's resource-dir <intrin.h>, which made the gate blind to the ONE platform macro
    # clang predefines for a triple we actually ship: this fixture passed with "0 divergences"
    # until <intrin.h> was stubbed instead. It gets its own case rather than riding on __GNUC__
    # for exactly that reason - __GNUC__ is undefined for the win triple, so it never exercised
    # the win-vs-linux leg at all (two of four "the gate missed it" findings were wrong fixtures).
    ("a _MSC_VER branch is two programs", "mscver",
     "#ifdef _MSC_VER\nstruct M { u32 a; };\n#else\nstruct M { u64 a; };\n#endif\n"
     "u32 m_use(void) { return (u32)sizeof(M); }\n",
     "preprocessed source differs"),
]
# The false positive the -U_MSC_VER flag was reaching for, and the evidence that stubbing
# <intrin.h> keeps it fixed: a vendor-shaped header that includes <intrin.h> under _MSC_VER pulls
# ~90 SIMD-intrinsic records into the win layout dump and none into linux/pi. The gate must see
# zero divergences here AND still fail the mscver case above - one flag could not do both.
TARGET_INTRIN = ("#ifdef _MSC_VER\n#include <intrin.h>\n#endif\n"
                 "struct Plain { u32 a; u64 b; };\n"
                 "u32 use(void) { return (u32)sizeof(Plain); }\n")
TARGET_CLEAN = ("struct OK { u32 a; u64 b; };\n"
                "// Ordinary sim code must not read as a divergence.\n"
                "inline u32 ok_sum(u32 a, u32 b) { return a + b; }\n"
                "static const u64 MASK = 0xFFFFFFFFULL;\n"
                "inline u64 ok_mask(u64 v) { return v & MASK; }\n")

# The fifth review's silent passes. Every one is a divergent record that the name-keyed comparison
# dropped instead of reporting - the same failure shape as the path filter that compared two empty
# lists. They are here so the positional comparison can never regress to keying by name.
TARGET_IDENTITY = [
    ("a record named like a system one is still compared", "sysname",
     "struct __Cell { u8 a : 4; u16 c : 8; };\n"
     "u32 use(void) { return (u32)sizeof(__Cell); }\n"),
    ("an anonymous-namespace record is still compared", "anonns",
     "namespace { struct R { u8 a : 4; u16 c : 8; }; }\n"
     "u32 use(void) { return (u32)sizeof(R); }\n"),
    ("an unnamed typedef'd record is still compared", "unnamed",
     "typedef struct { u8 a : 4; u16 c : 8; } T;\n"
     "u32 use(void) { return (u32)sizeof(T); }\n"),
    ("two function-local records with one name are both compared", "shadow",
     "u32 f1(void) { struct L { u8 a : 4; u16 c : 8; }; return (u32)sizeof(L); }\n"
     "u32 f2(void) { struct L { u32 x; }; return (u32)sizeof(L); }\n"),
]

# The 64-bit spellings and literal suffixes that made a realistic fx.h read as 18 divergences, none
# of them real. fx<i64,FRAC> dumps as fx<long long,..> on windows-msvc and fx<long,..> elsewhere.
TARGET_FX_HEADER = (
    "template <class Rep, u32 FRAC>\nstruct fx { Rep raw; };\n"
    "using pos_t = fx<i32, 18>;\n"
    "using wide_t = fx<i64, 32>;\n"
    "struct Body { pos_t x; pos_t y; wide_t accum; u64 mask; };\n"
    "inline u64 fx_all_ones(void) { return UINT64_C(0xFFFFFFFF); }\n"
    "inline u64 fx_big(void) { return 1000ULL; }\n"
    "u32 use(void) { return (u32)sizeof(Body); }\n")


def test_targets(tmp, cxx):
    root = fixture_root(tmp, "tgt")
    tool = os.path.join(AUDIT, "targets.py")

    def run_one(name, body):
        write(root, "src/sim/%s.cpp" % name,
              '#include "foundation/tl_types.h"\n' + body)
        rc, out = run([sys.executable, tool, "--root", root, "--clang", cxx,
                       "--only", "src/sim/%s.cpp" % name])
        os.remove(os.path.join(root, "src", "sim", "%s.cpp" % name))
        return rc, out

    for label, name, body, expect in TARGET_CASES:
        rc, out = run_one(name, body)
        record("targets: " + label, rc == 1 and expect in out, out.strip()[:200])

    for label, name, body in TARGET_IDENTITY:
        rc, out = run_one(name, body)
        record("targets: " + label, rc == 1 and "different layout" in out, out.strip()[:200])

    rc, out = run_one("clean", TARGET_CLEAN)
    record("targets: ordinary sim code is not a divergence", rc == 0, out.strip()[:300])

    rc, out = run_one("fxlike", TARGET_FX_HEADER)
    record("targets: a realistic fx.h is not 18 false positives", rc == 0, out.strip()[:400])

    rc, out = run_one("intrin", TARGET_INTRIN)
    record("targets: a vendor <intrin.h> include is not ~90 false divergences", rc == 0,
           out.strip()[:400])

    # The filter that reads our own lines out of the preprocessor output is the whole gate: when it
    # was wrong, every comparison was between two empty lists and everything passed.
    write(root, "src/sim/marker.cpp",
          '#include "foundation/tl_types.h"\nu32 marker(void) { return 7u; }\n')
    rc, out = run([sys.executable, tool, "--root", root, "--clang", cxx,
                   "--only", "src/sim/marker.cpp"])
    os.remove(os.path.join(root, "src", "sim", "marker.cpp"))
    record("targets: our own source reaches the comparison", rc == 0 and "line-marker" not in out,
           out.strip()[:200])


# --- commit_docs.py ---------------------------------------------------------------------------
def git(repo, *args):
    return run(["git", "-C", repo] + list(args))


def test_commit_docs(tmp):
    """A throwaway repo with two commits: one that changes a module without its doc, one that
    says [docs:none]. The gate had no fixture at all until the third W0 review said so."""
    repo = os.path.join(tmp, "cd")
    os.makedirs(repo, exist_ok=True)
    git(repo, "init", "-q")
    git(repo, "config", "user.email", "t@t")
    git(repo, "config", "user.name", "t")
    write(repo, "README.md", "seed\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "seed")
    base = git(repo, "rev-parse", "HEAD")[1].strip()

    write(repo, "src/sim/x.cpp", "int f(void) { return 1; }\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "touch sim without its doc")
    rc, out = run([sys.executable, os.path.join(AUDIT, "commit_docs.py"), "--base", base], cwd=repo)
    record("commit_docs: a module change with no doc change is refused",
           rc == 1 and "docs/ALLOY.md" in out, out.strip()[:200])

    # The waiver is per COMMIT. A later commit saying [docs:none] must not retroactively excuse
    # the undocumented one above it - the previous version of this fixture asserted that it did,
    # which is how the hole survived a review.
    write(repo, "src/sim/y.cpp", "int g(void) { return 2; }\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "touch sim again\n\n[docs:none]")
    rc, out = run([sys.executable, os.path.join(AUDIT, "commit_docs.py"), "--base", base], cwd=repo)
    record("commit_docs: a later [docs:none] does not waive an earlier commit",
           rc == 1 and "x.cpp" not in out and "src/sim/" in out, out.strip()[:200])

    mid = git(repo, "rev-parse", "HEAD~1")[1].strip()
    rc, out = run([sys.executable, os.path.join(AUDIT, "commit_docs.py"), "--base", mid], cwd=repo)
    record("commit_docs: [docs:none] waives its own commit", rc == 0, out.strip()[:200])

    write(repo, "src/sim/z.cpp", "int h(void) { return 3; }\n")
    write(repo, "docs/ALLOY.md", "# alloy\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "touch sim with its doc")
    head = git(repo, "rev-parse", "HEAD~1")[1].strip()
    rc, out = run([sys.executable, os.path.join(AUDIT, "commit_docs.py"), "--base", head], cwd=repo)
    record("commit_docs: a module change with its doc passes", rc == 0, out.strip()[:200])

    rc, out = run([sys.executable, os.path.join(AUDIT, "commit_docs.py"),
                   "--base", "0" * 40], cwd=repo)
    record("commit_docs: an all-zero base skips instead of crashing", rc == 0, out.strip()[:200])

    rc, out = run([sys.executable, os.path.join(AUDIT, "commit_docs.py"),
                   "--base", "deadbeef" * 5], cwd=repo)
    record("commit_docs: an unreachable base skips loudly instead of crashing",
           rc == 0 and "not in this clone" in out, out.strip()[:200])


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
        test_symbols_tooling(tmp, a.nm, a.objdump, a.ar, a.cxx)
        test_targets(tmp, a.cxx)
        test_tier_parity(tmp)
        test_fingerprint(tmp)
        test_commit_docs(tmp)

    failed = [r for r in results if not r[1]]
    print("selftest: %d checks, %d failed" % (len(results), len(failed)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
