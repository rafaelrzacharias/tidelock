// vmem.cpp - thin: the headless VMemApi is state->vmem_table, already the shared os_* table
// (docs/PLATFORM.md §9.4 "vmem, entropy, crash | the shared os_* TUs - bit-identical behaviour").
// No functions of its own - headless_apis.h exposes no separate accessor because PlatformApi.vmem
// is a VALUE, not a pointer (init.cpp copies state->vmem_table into it directly).
