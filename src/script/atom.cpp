// atom.cpp - the string-atom callback and the ONE pointer it needs. Contract: script/vm.h.
// Spec: docs/LUAU-LAYER.md §10.2 step 8 (`useratom`), §10.5 (what an atom buys: a field lookup
//   that compares a u16 instead of hashing a string on every access); docs/CANON.md "Types"
//   (`StrId` = u16, process-stable, never serialized).
//
// Alone in its TU, and named in tools/audit/static_allow.txt, for one reason: Luau's callback is
//   int16_t (*)(const char* s, size_t len)
// with no lua_State* and no userdata. There is no context to pass the process Interner through,
// and there never will be - the callback runs inside string interning, below the VM's own
// bookkeeping. RR-19 (ruled 2026-08-26) records the alternatives; every one of them either kept
// the hash-per-access cost or serialized a value CANON.md says is never serialized.
//
// Why one process-wide interner is the right shape anyway: `StrId` is process-stable BY
// DEFINITION (docs/CANON.md), so a second interner would mint a second numbering for the same
// name and the two VMs could not share a component's field ids. The pointer being global is not
// a compromise around the callback - it matches what the id already means.
#include <lua.h>

#include "foundation/tl_assert.h"
#include "script/vm.h"

// The exempted pointer (tools/audit/static_allow.txt, row `tl_script src/script atom`).
static Interner* g_atom_interner = nullptr;

// docs/LUAU-LAYER.md §10.2 step 8. Called by Luau whenever a string is created. Returns the
// string's StrId if it is ALREADY registered, else -1: registering here would mean any string a
// script constructed at runtime could grow the interner, and the interner is capped and
// fingerprint-adjacent. Only names registered at init (component fields, actions, tables, event
// types) get atoms, which is exactly the set the proxies look up.
static int16_t script_useratom(const char* s, size_t len) {
    Interner* in = g_atom_interner;
    if (in == nullptr) return -1;
    // A lookup, never an insert: intern() would ADD the string. The interner's by_hash map is
    // the lookup, and the StrId it returns is < 32767 by the cap CANON.md sets, so the cast is
    // always non-negative and -1 stays unambiguous.
    const StrView probe = StrView{ s, (u32)len };
    const NameHash h = sv_hash(probe);
    const StrId* existing = map_get(&in->by_hash, h);
    if (existing == nullptr) return -1;
    // intern() TL_FATALs when two DIFFERENT strings share a NameHash; a LOOKUP that skipped the
    // comparison would instead hand a runtime-built string the atom of the registered name it
    // collided with, and a proxy would then read the wrong field with nothing to see. Same check,
    // same contract - and here the answer is simply "not registered", because it is not.
    if (!sv_eq(intern_name(in, *existing), probe)) return -1;
    TL_ASSERT(*existing < 32767u);
    return (int16_t)*existing;
}

void script_install_useratom(lua_State* L, Interner* interner) {
    if (interner == nullptr) return;              // no interner, no atoms: lua_tostringatom gives -1
    if (g_atom_interner != nullptr && g_atom_interner != interner) {
        // Two interners would mint two numberings for one name, and a proxy built against one
        // would read the wrong field through the other. Loud, not last-one-wins.
        TL_FATAL("script_install_useratom: a different Interner is already installed - StrId is "
                 "process-stable by docs/CANON.md, so there is exactly one numbering");
    }
    g_atom_interner = interner;
    lua_callbacks(L)->useratom = &script_useratom;
}

const Interner* script_atom_interner(void) { return g_atom_interner; }
