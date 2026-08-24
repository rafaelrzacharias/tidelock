#pragma once
// ---------------------------------------------------------------------------------------------
// driver_args.h - tl_driver's command line: the DriverArgs record, the parser and its closed
//   error set. Spec: docs/TESTING.md §9.2 (the flag list and the mutual exclusions).
// Why it is a header and not part of main.cpp: the parser is the driver's whole contract today -
//   which flags are required, which pairs are mutually exclusive, which values are numbers - and
//   it shipped inside a TU with `main`, so no test could reach it (W1 runner review 2).
//   tests/driver/driver_args.test.cpp is the reason it moved.
// Invariants: pure. No io, no allocation, no static state. A rejected command line leaves `out`
//   at driver_args_default() - never partially filled (docs/CPP-SUBSET.md §3: no partial states).
// Determinism: the parse is a pure function of argv; no environment, no cwd, no clock.
// Threading: none.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"

#include <string.h>   // strcmp - the io-exempt half of docs/TESTING.md §8 R-2

// tl_driver's ErrCode range (docs/CPP-SUBSET.md §3: a closed enum : u16 per module, 0 = OK, with
// a constexpr name table for the message - errors never carry strings).
constexpr ErrCode DRV_ERR_UNKNOWN_FLAG      = (ErrCode)0x0201;
constexpr ErrCode DRV_ERR_MISSING_VALUE     = (ErrCode)0x0202;
constexpr ErrCode DRV_ERR_NOT_A_NUMBER      = (ErrCode)0x0203;
constexpr ErrCode DRV_ERR_EMPTY_VALUE       = (ErrCode)0x0204;
constexpr ErrCode DRV_ERR_NO_SCENE          = (ErrCode)0x0205;
constexpr ErrCode DRV_ERR_NO_SEED           = (ErrCode)0x0206;
constexpr ErrCode DRV_ERR_NO_TICKS          = (ErrCode)0x0207;
constexpr ErrCode DRV_ERR_WORKERS_CONFLICT  = (ErrCode)0x0208;
constexpr ErrCode DRV_ERR_RECORD_REPLAY     = (ErrCode)0x0209;
constexpr ErrCode DRV_ERR_VERIFY_NO_REPLAY  = (ErrCode)0x020a;
constexpr ErrCode DRV_ERR_BAD_WORKERS       = (ErrCode)0x020b;
constexpr ErrCode DRV_ERR_BAD_TICKS         = (ErrCode)0x020c;
constexpr ErrCode DRV_ERR_BAD_SWEEP         = (ErrCode)0x020d;
constexpr ErrCode DRV_ERR_BAD_SNAPSHOT      = (ErrCode)0x020e;

inline const char* drv_err_name(ErrCode e) {
    switch (e) {
        case ERR_OK:                   return "ok";
        case DRV_ERR_UNKNOWN_FLAG:     return "unrecognised argument";
        case DRV_ERR_MISSING_VALUE:    return "flag needs a value";
        case DRV_ERR_NOT_A_NUMBER:     return "value is not a non-negative decimal integer";
        case DRV_ERR_EMPTY_VALUE:      return "value must not be empty";
        case DRV_ERR_NO_SCENE:         return "--scene is required";
        case DRV_ERR_NO_SEED:          return "--seed is required";
        case DRV_ERR_NO_TICKS:         return "--ticks is required";
        case DRV_ERR_WORKERS_CONFLICT: return "--workers and --workers-sweep are mutually exclusive";
        case DRV_ERR_RECORD_REPLAY:    return "--record and --replay are mutually exclusive";
        case DRV_ERR_VERIFY_NO_REPLAY: return "--verify requires --replay";
        case DRV_ERR_BAD_WORKERS:      return "--workers must be >= 1";
        case DRV_ERR_BAD_TICKS:        return "--ticks must be >= 1";
        case DRV_ERR_BAD_SWEEP:        return "--workers-sweep must be a comma-separated list of counts >= 1";
        case DRV_ERR_BAD_SNAPSHOT:     return "--snapshot-every must be >= 1";
        default:                       return "unknown tl_driver error";
    }
}

struct DriverArgs {
    const char* scene;
    i64 seed;                  // -1 = not given
    i64 ticks;                 // -1 = not given
    i64 workers;               // -1 = not given
    const char* workers_sweep;
    const char* record_path;
    const char* replay_path;
    bool verify;
    bool dual;
    const char* dump_probes_dir;
    const char* csv_path;
    i64 snapshot_every;        // -1 = not given
    i64 ballast_bytes;         // -1 = not given
};

inline DriverArgs driver_args_default(void) {
    DriverArgs a = {};
    a.seed = -1; a.ticks = -1; a.workers = -1; a.snapshot_every = -1; a.ballast_bytes = -1;
    return a;
}

// Strict non-negative decimal parse into `out`, true on success. atoll was used here and answers
// 0 for "abc", "" and "12x", so `--ticks abc` was a silent zero-tick run and `--seed abc` a
// silent seed 0 - a driver whose contract is "byte-identical output for the same seed"
// (docs/TESTING.md §9.2) must never guess which seed it was told. Overflow is rejected, not
// wrapped: every numeric flag the driver takes is a count or a size.
inline bool drv_parse_u63(const char* s, i64* out) {
    if (s == nullptr || *s == 0) { return false; }
    constexpr i64 I64_MAX = (i64)0x7fffffffffffffff;
    i64 v = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') { return false; }
        const i64 digit = (i64)(*p - '0');
        if (v > I64_MAX / 10) { return false; }
        v *= 10;
        if (v > I64_MAX - digit) { return false; }
        v += digit;
    }
    *out = v;
    return true;
}

// "1,2,8,16" - a non-empty comma-separated list of counts >= 1: no spaces, no empty elements, no
// trailing comma. It was accepted unvalidated, so `--workers-sweep abc` reached the boot as if it
// were a sweep plan, and a sweep that silently runs one configuration is a determinism gate that
// swept nothing (docs/TESTING.md §3).
inline bool drv_parse_sweep(const char* s) {
    if (s == nullptr || *s == 0) { return false; }
    i64 n = 0;
    bool in_element = false;
    for (const char* p = s;; ++p) {
        if (*p >= '0' && *p <= '9') {
            n = n * 10 + (i64)(*p - '0');
            if (n > 0x7fffffff) { return false; }
            in_element = true;
        } else if (*p == ',' || *p == 0) {
            if (!in_element || n < 1) { return false; }
            if (*p == 0) { return true; }
            n = 0; in_element = false;
        } else {
            return false;
        }
    }
}

// The flag sweep alone: fills `a`, or names the first argument it could not take.
inline ErrCode driver_scan_flags(int argc, char** argv, DriverArgs* a, const char** bad) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char** str_dst = nullptr;
        i64* num_dst = nullptr;
        if      (strcmp(arg, "--scene") == 0)          { str_dst = &a->scene; }
        else if (strcmp(arg, "--workers-sweep") == 0)  { str_dst = &a->workers_sweep; }
        else if (strcmp(arg, "--record") == 0)         { str_dst = &a->record_path; }
        else if (strcmp(arg, "--replay") == 0)         { str_dst = &a->replay_path; }
        else if (strcmp(arg, "--dump-probes") == 0)    { str_dst = &a->dump_probes_dir; }
        else if (strcmp(arg, "--csv") == 0)            { str_dst = &a->csv_path; }
        else if (strcmp(arg, "--seed") == 0)           { num_dst = &a->seed; }
        else if (strcmp(arg, "--ticks") == 0)          { num_dst = &a->ticks; }
        else if (strcmp(arg, "--workers") == 0)        { num_dst = &a->workers; }
        else if (strcmp(arg, "--snapshot-every") == 0) { num_dst = &a->snapshot_every; }
        else if (strcmp(arg, "--ballast") == 0)        { num_dst = &a->ballast_bytes; }
        else if (strcmp(arg, "--verify") == 0)         { a->verify = true; continue; }
        else if (strcmp(arg, "--dual") == 0)           { a->dual = true; continue; }
        else { *bad = arg; return DRV_ERR_UNKNOWN_FLAG; }

        if (i + 1 >= argc) { *bad = arg; return DRV_ERR_MISSING_VALUE; }
        const char* value = argv[++i];
        if (str_dst) {
            if (value[0] == 0) { *bad = arg; return DRV_ERR_EMPTY_VALUE; }
            *str_dst = value;
        } else {
            if (!drv_parse_u63(value, num_dst)) { *bad = value; return DRV_ERR_NOT_A_NUMBER; }
        }
    }
    return ERR_OK;
}

// The rules that need the whole command line (docs/TESTING.md §9.2). Order matters only for the
// message a user sees; every rule is checked before the driver would boot anything.
inline ErrCode driver_validate(const DriverArgs& a, const char** bad) {
    if (a.scene == nullptr)                { return DRV_ERR_NO_SCENE; }
    if (a.seed < 0)                        { return DRV_ERR_NO_SEED; }
    if (a.ticks < 0)                       { return DRV_ERR_NO_TICKS; }
    // A zero-tick run would boot, step nothing, write an empty CSV and exit 0 - a determinism
    // job that verified nothing while reporting success (docs/TESTING.md §1). --ticks is a count.
    if (a.ticks == 0)                      { return DRV_ERR_BAD_TICKS; }
    if (a.workers >= 0 && a.workers_sweep) { return DRV_ERR_WORKERS_CONFLICT; }
    if (a.record_path && a.replay_path)    { return DRV_ERR_RECORD_REPLAY; }
    if (a.verify && !a.replay_path)        { return DRV_ERR_VERIFY_NO_REPLAY; }
    if (a.workers == 0)                    { return DRV_ERR_BAD_WORKERS; }
    if (a.workers_sweep && !drv_parse_sweep(a.workers_sweep)) { *bad = a.workers_sweep; return DRV_ERR_BAD_SWEEP; }
    if (a.snapshot_every == 0)             { return DRV_ERR_BAD_SNAPSHOT; }
    return ERR_OK;
}

// Parses and validates argv into `out`. Returns ERR_OK, or the one rule that rejected it; `out`
// is driver_args_default() on any error. `bad_arg_out`, when non-null, receives the offending
// argument for the message (null when the rule is about the command line as a whole).
inline ErrCode driver_parse_args(int argc, char** argv, DriverArgs* out, const char** bad_arg_out) {
    DriverArgs a = driver_args_default();
    const char* bad = nullptr;
    ErrCode err = driver_scan_flags(argc, argv, &a, &bad);
    if (err == ERR_OK) { err = driver_validate(a, &bad); }
    if (bad_arg_out) { *bad_arg_out = bad; }
    *out = (err == ERR_OK) ? a : driver_args_default();
    return err;
}
