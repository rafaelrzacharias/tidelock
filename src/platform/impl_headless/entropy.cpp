// entropy.cpp - thin: the headless EntropyApi is state->entropy_table, filled directly by
// os_entropy_fill_table() in init.cpp (docs/PLATFORM.md §9.4 "vmem, entropy, crash | the shared
// os_* TUs - bit-identical behaviour to sdl3"). No functions of its own - PlatformApi.entropy is
// `const EntropyApi*`, so init.cpp just points it at &state->entropy_table.
