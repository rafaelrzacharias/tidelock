# tidelock — lessons & gotchas (one line each; read before build work)

- **IDE = VS Code + CMake Tools (presets) + clangd + CodeLLDB**; no Visual Studio solutions. `.vscode/` is committed; put machine-local tweaks in `.vscode/*.local.json` (ignored). Toolchain on this PC: LLVM 22.1.7 (`C:\Program Files\LLVM\bin`), CMake 4.3, Ninja 1.13, Python 3.13.
- clangd reads `.cache/compile_commands.json`, which CMake Tools copies from the active preset — configure once before expecting diagnostics.
- Binaries land in `out/<preset>/bin/`; `out/` and `.cache/` are ignored.
- **clang-cl is a `cl` driver, not a `clang` driver**: `-Wall` there means MSVC's `/Wall`, which clang maps to `-Weverything` (`-Wc++98-compat` on every C++20 line). Use `/W4 /WX`, `/EHs-c-`, `/GR-`, `/Zc:threadSafeInit-`, `/clang:-nostdinc++`; the other `-W`/`-f` flags are the same in both modes (`docs/BUILD.md` §9 R-6).
- `CMAKE_BUILD_TYPE` is forced empty — tiers own the flags. Leave it and CMake silently appends its own `/Ob0 /Od /RTC1 -MDd` set, which is a `build_id` input nobody chose (`docs/BUILD.md` §9 R-5).
- CMake presets are only read from `CMakePresets.json` at the repo root; ours is two lines that `include` `cmake/presets.json`.
- In CMake regexes write parentheses as `[(]` / `[)]`: quoted-argument escaping and the regex engine disagree about backslashes, and a mis-escaped pattern silently matches nothing (it cost a build cycle in the test-list generator).
- A static lib needs at least one TU: every empty module folder carries one placeholder `.cpp` until its lane lands. Module sources are globbed with `CONFIGURE_DEPENDS`, so a lane adds files without editing another lane's CMake.
- `tl_foundation_det` vs `tl_foundation` is split by an explicit NON-det stem list — anything new defaults to the audited half, so a mistake fails the symbol audit loudly instead of escaping it.
- An audit is worth what its **negative test** is worth. The W0 gates passed every obvious plant and let five adversarial ones through (`f32` instead of `float`, a basename collision across exempt lists, an anonymous-namespace global, an upward `#include`, an upward symbol reference). Plant the clever violation, not the obvious one.
- Regexes cannot see mutable globals — anonymous namespaces, `inline static`, dynamic initialisers all evade them. `llvm-objdump -h` and a zero-`.data`/`.bss` rule can, and cost nothing.
- `build_id` over a *flag string* is a lie: compile definitions, `-std`, `CXXFLAGS` from the environment, `cmake/` edits and git-ignored sources all bypass it. Hash the resolved compile commands (`compile_commands.json`) plus the content of the files they name.
- `[[nodiscard]]` cannot attach to a typedef. An error code that must not be discarded has to be an `enum`, not `using ErrCode = u16`.
- A doc sentence that names no tool ("the fingerprint tool asserts it") is a phantom gate; grep for the tool before believing it.
- `build_id` had to answer one question, and it was answering two: "are we the same program?" (must be target-independent, or PC + Deck + Pi peers can never hand-shake) and "what exactly built this binary?" (compiler, triple, flags). Two questions, two values: `build_id` is compared, `build_env` is reported (`docs/BUILD.md` §9 R-8).
- clang spells it `-std=c++20`, clang-cl `-std:c++20`. Normalise driver spellings before they reach a hash, or two targets disagree on a value that is supposed to be identical.
- The audit selftest found four bugs in its first run - two in the fixtures, one in the new `build_id`, one real gate hole (a header's contract block was counting as the first function's contract comment). Write the negative test the same day as the gate.
