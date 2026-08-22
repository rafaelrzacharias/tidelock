# tidelock — lessons & gotchas (one line each; read before build work)

- **IDE = VS Code + CMake Tools (presets) + clangd + CodeLLDB**; no Visual Studio solutions. `.vscode/` is committed; put machine-local tweaks in `.vscode/*.local.json` (ignored). Toolchain on this PC: LLVM 22.1.7 (`C:\Program Files\LLVM\bin`), CMake 4.3, Ninja 1.13, Python 3.13.
- clangd reads `.cache/compile_commands.json`, which CMake Tools copies from the active preset — configure once before expecting diagnostics.
- Binaries land in `out/<preset>/bin/`; `out/` and `.cache/` are ignored.
