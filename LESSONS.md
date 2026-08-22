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
