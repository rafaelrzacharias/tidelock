#pragma once
// ---------------------------------------------------------------------------------------------
// action_map.h - Action/Binding/ActionMap: the game-declared action vocabulary and its device
//   bindings (docs/INPUT.md §2).
//
// Spec: docs/INPUT.md §2 (design), §3 (contexts), §9.2 (this header's structures), §9.3
//   (the Live producer's fold - core/producers/live.cpp consumes this header).
// Purpose: the game (via Luau, later) declares actions and binds devices to them; the Live
//   producer's fold resolves bindings against raw device state into an InputFrame every tick
//   (docs/INPUT.md §0). Registration here is the plain C++-native door (TL_FATAL on a bad call,
//   matching reflect.h's TL_COMPONENT door) - a future Luau binding wraps it with a Result-
//   returning validated door (the world_register_component_luau precedent, docs/ECS.md §10.7),
//   not built by this lane (LUAU-LAYER.md's binding layer does not exist yet).
// Invariants: action ids are dense (registration order, < MAX_ACTIONS - docs/INPUT.md §2).
//   `action_map_fingerprint` hashes (name, kind, cls) per action in registration order ONLY -
//   bindings are local preference and are never fingerprinted (docs/INPUT.md §2: "Bindings are
//   data... they never affect what is transmitted, only how this peer produces it"). Binding
//   resolution order is most-specific-modifiers-first within one context (docs/INPUT.md §9.3);
//   ties (equal modifier-bit count) resolve in binding-registration order, deterministically.
// Determinism: `action_map_fingerprint` is a pure function of registered actions, used only for
//   the build/session fingerprint (never for gameplay); resolution itself runs inside the Live
//   producer, which is real-time/non-deterministic BY DESIGN (this peer's own input) - the
//   determinism boundary is the InputFrame it produces, not this file (docs/INPUT.md §0/§1).
// Threading: single-threaded; action_register/action_bind are init-only (TL_FATAL after the map
//   is built - "built" here means the caller stops registering, there is no explicit seal step
//   since nothing else in the tree reads ActionMap before the Live producer starts folding).
// Includes: foundation/{tl_types,tl_assert,hash,interner,array,vmem_arena}.h, core/input.h.
// ---------------------------------------------------------------------------------------------
#include "foundation/tl_types.h"
#include "foundation/tl_assert.h"
#include "foundation/hash.h"
#include "foundation/interner.h"
#include "foundation/array.h"
#include "foundation/vmem_arena.h"
#include "core/input.h"

// docs/INPUT.md §9.2. DIGITAL actions produce {0,1}; ANALOG actions produce the full -127..127
// range.
enum ActionKind : u8 { ACT_DIGITAL = 0, ACT_ANALOG = 1 };

// docs/INPUT.md §2: the netcode substitution policy (docs/NETCODE.md §8.4) - declared here
// because it is a property of the ACTION, shipped identically to every peer. LATCHED = hold
// forward on substitution, AXIS = decay to neutral over SUB_DECAY_TICKS, EDGE = null.
enum ActionClass : u8 { CLS_LATCHED = 0, CLS_AXIS = 1, CLS_EDGE = 2 };

// docs/INPUT.md §9.2, field for field (reordered for natural packing - no implicit compiler pad).
struct Action {
    NameHash    name;      //  0, 8 - the persistence/wire identity (docs/INPUT.md §2)
    StrId       sid;       //  8, 2 - process-stable interned name, for display only
    ActionKind  kind;      // 10, 1
    ActionClass cls;       // 11, 1
    u32         _pad0;     // 12, 4
};
static_assert(sizeof(Action) == 16u, "docs/INPUT.md section 9.2, explicit padding");

// docs/INPUT.md §9.2/§9.3: which raw device channel a binding reads.
enum BindDevice : u8 {
    DEV_KEY = 0, DEV_MOUSE_BUTTON = 1, DEV_MOUSE_AXIS = 2, DEV_KEYS_AXIS = 3,
    DEV_PAD_BUTTON = 4, DEV_PAD_AXIS = 5,
};

// docs/INPUT.md §2: deadzone shapes, applied before quantization.
enum Deadzone : u8 { DZ_NONE = 0, DZ_AXIAL = 1, DZ_RADIAL = 2, DZ_TRIGGER = 3 };

// docs/INPUT.md §2: SOCD cleaning for a keys-axis binding (two keys, one axis).
enum Socd : u8 { SOCD_NEUTRAL = 0, SOCD_LAST_WINS = 1, SOCD_FIRST_WINS = 2 };

// docs/INPUT.md §9.2, reordered for natural packing (no implicit compiler pad; the doc's own
// field order was pseudocode, not a pinned layout - this struct is never wire/hashed state).
// `modifiers`: bit0 shift, bit1 ctrl, bit2 alt, bit3 gui (matches platform.h RawEvent.key.mods).
// `code_neg`/`code_pos`: DEV_KEY/DEV_MOUSE_BUTTON/DEV_PAD_BUTTON/DEV_PAD_AXIS use code_pos as
// "the code" (a scancode, mouse button id, pad button id, or pad axis id); DEV_KEYS_AXIS uses
// both (the negative and positive key's scancodes); DEV_MOUSE_AXIS ignores both (there is only
// one mouse). `context`: which context (docs/INPUT.md §3) this binding is live under.
struct Binding {
    ActionId   action;      //  0, 2
    BindDevice dev;         //  2, 1
    u8         modifiers;   //  3, 1
    u16        code_neg;    //  4, 2
    u16        code_pos;    //  6, 2
    Deadzone   dz;          //  8, 1
    Socd       socd;        //  9, 1
    u8         context;     // 10, 1
    u8         _pad0;       // 11, 1
    f32        dz_radius;   // 12, 4
    f32        sensitivity; // 16, 4
};
static_assert(sizeof(Binding) == 20u, "docs/INPUT.md section 9.2, explicit padding");

// docs/INPUT.md §3: v0 ships one context; the context stack is reserved.
enum : u8 { CONTEXT_DEFAULT = 0 };

// docs/INPUT.md §9.2. `bindings` is a fixed-capacity Array (action_map_init sizes it) - a small,
// bounded table built once at init, never grown mid-run.
struct ActionMap {
    Action        actions[MAX_ACTIONS];
    u32           action_count;
    u8            active_context;
    u8            _pad0[3];
    Array<Binding> bindings;
};

// Sizes `bindings` from `arena` (a fixed array_init_fixed, docs/CONTAINERS.md §8.1); zeroes the
// rest. `max_bindings` is the caller's budget - TL_FATAL on overflow at action_bind time, not here.
void action_map_init(ActionMap* m, VMemArena* arena, u32 max_bindings);

// Registers one action (docs/INPUT.md §2's `input.action(name, kind, class)`). Returns the dense
// ActionId (registration order). TL_FATAL: MAX_ACTIONS reached, or `name` already registered
// (duplicate names would make the wire/fingerprint identity ambiguous).
ActionId action_register(ActionMap* m, NameHash name, StrId sid, ActionKind kind, ActionClass cls);

// The registered-name lookup (docs/INPUT.md §2's persistence identity); ACTION_ID_NONE if absent.
// Pure, cold-path (linear scan - MAX_ACTIONS is 32).
ActionId action_find(const ActionMap* m, NameHash name);

// Adds one binding (docs/INPUT.md §2's `input.bind(...)`). TL_CHECK(b.action < m->action_count).
// TL_FATAL on `bindings` overflow (array_push's fixed-array contract).
void action_bind(ActionMap* m, const Binding& b);

// Sets the active context (docs/INPUT.md §3). Takes effect at the NEXT fold call - the Live
// producer detects the change at the top of its fold and emits synthetic release edges for any
// held action whose binding set the switch invalidates (docs/INPUT.md §9.3).
void action_map_set_context(ActionMap* m, u8 context);

// docs/INPUT.md §2: "the registered action list (names + kinds + classes, in order) hashes into
// the build fingerprint". Field-wise (never raw struct bytes, matching reflect.h's
// tl_reflect_component_hash pattern) so struct padding never enters the hash. Bindings are NOT
// included (docs/INPUT.md §2: "local preference... never affect what is transmitted"). Pure.
u64 action_map_fingerprint(const ActionMap* m);
