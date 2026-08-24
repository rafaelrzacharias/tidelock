// tl_driver - the headless driver skeleton (docs/TESTING.md §9.2, W1 runner+driver lane). Flag
// parsing and validation against the real contract live in driver_args.h (and are tested by
// tests/driver/driver_args.test.cpp); the boot itself is stubbed until the platform lane's
// headless impl (src/platform/impl_headless - still a placeholder TU, docs/PLATFORM.md §9) and
// app/wiring.cpp (docs/ARCHITECTURE.md §9, not built yet) exist. Tests are not sim code:
// printf-class io/clock/filesystem is the exemption of docs/TESTING.md §8 R-2.
//
// Exit codes (docs/TESTING.md §9.2, the REAL contract this file parses/validates against but
// cannot yet fulfil):
//   0 = the run completed and, if --verify was given, every hash matched.
//   3 = a divergence was found at the tick printed to stderr.
//   1 = a flag-parsing/validation error (this file's own contract - §9.2 does not name one, and
//       a malformed invocation is not a divergence).
//  70 = EX_SOFTWARE: boot not implemented yet (the stub_main.cpp convention, kept for continuity
//       - "not implemented" must never look like exit 0/3 of the real contract, or a CI job
//       reading this exit code would think a scene actually ran clean).
#include "driver/driver_args.h"
#include "foundation/tl_types.h"

#include <stdio.h>

static void usage(void) {
    fprintf(stderr,
        "tl_driver --scene <luau> --seed <n> --ticks <n> [--workers n | --workers-sweep 1,2,8,16]\n"
        "          [--record out.tlri | --replay in.tlri --verify] [--dual] [--dump-probes dir]\n"
        "          [--csv out] [--snapshot-every n] [--ballast bytes]   (docs/TESTING.md section 9.2)\n");
}

// The boot this skeleton cannot perform yet: headless platform init, app/wiring.cpp's scene
// load, the Script/Replay producer, engine_tick_once * ticks, CSV + hash output. STUB (TODO.md):
// wire the real sequence the day src/platform/impl_headless and app/wiring.cpp exist; every
// DriverArgs field is already the shape that call will take.
static bool driver_boot_headless_STUB(const DriverArgs& args) {
    (void)args;
    return false;   // never implemented yet - see the exit-code contract in the file header
}

int main(int argc, char** argv) {
    DriverArgs args = driver_args_default();
    const char* bad = nullptr;
    const ErrCode err = driver_parse_args(argc, argv, &args, &bad);
    if (err != ERR_OK) {
        if (bad) { fprintf(stderr, "tl_driver: %s ('%s')\n", drv_err_name(err), bad); }
        else     { fprintf(stderr, "tl_driver: %s\n", drv_err_name(err)); }
        usage();
        return 1;
    }

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
