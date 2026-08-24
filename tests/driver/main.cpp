// tl_driver - the headless driver skeleton (docs/TESTING.md §9.2, W1 runner+driver lane). Full
// flag parsing and validation against the real contract lands now; the boot itself is stubbed
// until the platform lane's headless impl (src/platform/impl_headless - still a placeholder TU,
// docs/PLATFORM.md §9) and app/wiring.cpp (docs/ARCHITECTURE.md §9, not built yet) exist. Tests
// are not sim code: printf-class io/clock/filesystem is the exemption of docs/TESTING.md §8 R-2.
//
// Exit codes (docs/TESTING.md §9.2, the REAL contract this file parses/validates against but
// cannot yet fulfil):
//   0 = the run completed and, if --verify was given, every hash matched.
//   3 = a divergence was found at the tick printed to stderr.
//   1 = a flag-parsing/validation error (this file's own contract - §9.2 does not name one, and
//       a malformed invocation is not a divergence).
//  70 = EX_SOFTWARE: boot not implemented yet (the docs/TESTING.md §8's stub_main.cpp convention,
//       kept for continuity - "not implemented" must never look like exit 0/3 of the real
//       contract, or a CI job reading this exit code would think a scene actually ran clean).
#include "foundation/tl_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct DriverArgs {
    const char* scene = nullptr;
    i64 seed = -1;
    i64 ticks = -1;
    i64 workers = -1;                 // -1 = not given
    const char* workers_sweep = nullptr;
    const char* record_path = nullptr;
    const char* replay_path = nullptr;
    bool verify = false;
    bool dual = false;
    const char* dump_probes_dir = nullptr;
    const char* csv_path = nullptr;
    i64 snapshot_every = -1;
    i64 ballast_bytes = -1;
};

static void usage(void) {
    fprintf(stderr,
        "tl_driver --scene <luau> --seed <n> --ticks <n> [--workers n | --workers-sweep 1,2,8,16]\n"
        "          [--record out.tlri | --replay in.tlri --verify] [--dual] [--dump-probes dir]\n"
        "          [--csv out] [--snapshot-every n] [--ballast bytes]   (docs/TESTING.md section 9.2)\n");
}

// Parses argv into `out`. Returns false (and prints why) on a malformed invocation - never
// partially fills `out` and proceeds.
static bool parse_args(int argc, char** argv, DriverArgs* out) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool has_next = (i + 1) < argc;
        if (strcmp(a, "--scene") == 0 && has_next) { out->scene = argv[++i]; }
        else if (strcmp(a, "--seed") == 0 && has_next) { out->seed = atoll(argv[++i]); }
        else if (strcmp(a, "--ticks") == 0 && has_next) { out->ticks = atoll(argv[++i]); }
        else if (strcmp(a, "--workers") == 0 && has_next) { out->workers = atoll(argv[++i]); }
        else if (strcmp(a, "--workers-sweep") == 0 && has_next) { out->workers_sweep = argv[++i]; }
        else if (strcmp(a, "--record") == 0 && has_next) { out->record_path = argv[++i]; }
        else if (strcmp(a, "--replay") == 0 && has_next) { out->replay_path = argv[++i]; }
        else if (strcmp(a, "--verify") == 0) { out->verify = true; }
        else if (strcmp(a, "--dual") == 0) { out->dual = true; }
        else if (strcmp(a, "--dump-probes") == 0 && has_next) { out->dump_probes_dir = argv[++i]; }
        else if (strcmp(a, "--csv") == 0 && has_next) { out->csv_path = argv[++i]; }
        else if (strcmp(a, "--snapshot-every") == 0 && has_next) { out->snapshot_every = atoll(argv[++i]); }
        else if (strcmp(a, "--ballast") == 0 && has_next) { out->ballast_bytes = atoll(argv[++i]); }
        else { fprintf(stderr, "tl_driver: unrecognised argument '%s'\n", a); return false; }
    }
    if (!out->scene) { fprintf(stderr, "tl_driver: --scene is required\n"); return false; }
    if (out->seed < 0) { fprintf(stderr, "tl_driver: --seed is required\n"); return false; }
    if (out->ticks < 0) { fprintf(stderr, "tl_driver: --ticks is required\n"); return false; }
    if (out->workers >= 0 && out->workers_sweep) {
        fprintf(stderr, "tl_driver: --workers and --workers-sweep are mutually exclusive\n"); return false;
    }
    if (out->record_path && out->replay_path) {
        fprintf(stderr, "tl_driver: --record and --replay are mutually exclusive\n"); return false;
    }
    if (out->verify && !out->replay_path) {
        fprintf(stderr, "tl_driver: --verify requires --replay\n"); return false;
    }
    if (out->workers == 0) { fprintf(stderr, "tl_driver: --workers must be >= 1\n"); return false; }
    return true;
}

// The boot this skeleton cannot perform yet: headless platform init, app/wiring.cpp's scene
// load, the Script/Replay producer, engine_tick_once * ticks, CSV + hash output. STUB (TODO.md):
// wire the real sequence the day src/platform/impl_headless and app/wiring.cpp exist; every
// DriverArgs field above is already the shape that call will take.
static bool driver_boot_headless_STUB(const DriverArgs& args) {
    (void)args;
    return false;   // never implemented yet - see the exit-code contract in the file header
}

int main(int argc, char** argv) {
    DriverArgs args;
    if (!parse_args(argc, argv, &args)) { usage(); return 1; }

    if (!driver_boot_headless_STUB(args)) {
        fprintf(stderr,
            "tl_driver: flags parsed and validated for scene '%s' (seed %lld, ticks %lld) but the "
            "headless boot is not implemented yet - the platform lane's headless impl and "
            "app/wiring.cpp have not landed (docs/PLATFORM.md section 9, docs/TESTING.md section "
            "9.2). This is EX_SOFTWARE (70), not exit 0/3 of the real contract.\n",
            args.scene, (long long)args.seed, (long long)args.ticks);
        return 70;
    }
    return 0;   // unreachable until driver_boot_headless_STUB is replaced
}
