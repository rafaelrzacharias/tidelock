// inspector.cpp - see inspector.h's contract block. Spec: docs/TOOLING.md §9.3.4.
#include "editor/inspector.h"

#include "core/column.h"
#include "core/reflect.h"
#include "foundation/handle.h"
#include "foundation/interner.h"
#include "foundation/tl_assert.h"

#include <imgui.h>

#include <string.h>

namespace {

// The nine fx palette rows' FRAC bit counts (foundation/fx_palette.h's own row constants,
// docs/FX-PALETTE.md) - display only (this file's Invariants note: no RNE quantizer exists yet
// to accept an edited value back).
u8 fx_frac_bits(FieldKind k) {
    switch (k) {
        case K_pos:     return 18u;
        case K_vel:     return 20u;
        case K_invmass: return 18u;
        case K_stiff:   return 30u;
        case K_q:       return 30u;
        case K_angle:   return 30u;
        case K_omega:   return 22u;
        case K_dt:      return 30u;
        case K_scalar:  return 16u;
        default:        TL_FATAL("fx_frac_bits: not an fx kind");
    }
}

bool is_fx_kind(FieldKind k) { return k >= K_pos && k <= K_scalar; }
bool is_int_kind(FieldKind k) { return k >= K_i8 && k <= K_u64; }
bool is_handle_kind(FieldKind k) { return k >= K_Entity && k <= K_Basin; }

ImGuiDataType int_data_type(FieldKind k) {
    switch (k) {
        case K_i8:  return ImGuiDataType_S8;
        case K_u8:  return ImGuiDataType_U8;
        case K_i16: return ImGuiDataType_S16;
        case K_u16: return ImGuiDataType_U16;
        case K_i32: return ImGuiDataType_S32;
        case K_u32: return ImGuiDataType_U32;
        case K_i64: return ImGuiDataType_S64;
        case K_u64: return ImGuiDataType_U64;
        default:    TL_FATAL("int_data_type: not an integer kind");
    }
}

// The handle kind's display domain name (docs/TOOLING.md §9.3.4: "Text(domain #idx gN)"). Only
// K_Entity is reachable today (reflect.h's own comment: the other eleven have no
// tl_field_kind_<token> constant yet, so no component can declare a field of those kinds) - the
// rest are named here so the switch is complete the day a domain owner adds one, not left to
// silently fall into "unreachable" and TL_FATAL in a dev tool over a field that genuinely exists.
const char* handle_domain_name(FieldKind k) {
    switch (k) {
        case K_Entity:     return "entity";
        case K_Tex:        return "tex";
        case K_Font:       return "font";
        case K_Audio:      return "audio";
        case K_Clip:       return "clip";
        case K_Data:       return "data";
        case K_Body:       return "body";
        case K_Constraint: return "constraint";
        case K_Agent:      return "agent";
        case K_Plant:      return "plant";
        case K_Cavity:     return "cavity";
        case K_Basin:      return "basin";
        default:           TL_FATAL("handle_domain_name: not a handle kind");
    }
}

// Reads a handle field's raw bits at `esz` width (2 or 4 B, kind_scalar_size) into a u32 for
// display - every handle domain's Handle<> is index+gen packed into its own width, and
// handle_index/handle_gen only need the bit pattern, not the domain's own C++ type.
u32 load_handle_bits(const void* addr, u32 esz) {
    u32 bits = 0;
    memcpy(&bits, addr, esz);
    return bits;
}

// A dot-path-shaped {idx, gen} extraction over the raw bits, matching foundation/handle.h's own
// Handle<H, IDX_BITS, GEN_BITS> layout for the two shapes docs/CANON.md pins (Entity 22/10,
// resources 12/4) - display only, so an exact bit-count per domain is not needed: the low bits
// are always the index, the bits above are always the generation, and 22/10 covers every kind
// this file can reach (kind_scalar_size caps every handle domain at 4 B).
void handle_idx_gen(u32 bits, u32 esz, u32* idx, u32* gen) {
    if (esz == 2u) {
        *idx = bits & 0x0FFFu;         // resources: Handle<_, 12, 4>
        *gen = (bits >> 12) & 0x0Fu;
    } else {
        *idx = bits & 0x003FFFFFu;     // Entity: Handle<EntityTag, 22, 10>
        *gen = (bits >> 22) & 0x03FFu;
    }
}

void draw_field(Editor* ed, World* w, Entity sel, ComponentId comp, const ComponentInfo* info,
                 u32 fi, void* row) {
    const FieldInfo& f = info->fields[fi];
    const u32 esz = kind_scalar_size(f.kind);
    const u32 elems = f.count;   // reflect.h: "1, or the array length" - never 0
    const bool editable = (elems == 1u) && (is_int_kind(f.kind) || f.kind == K_bool);

    for (u32 k = 0; k < elems; ++k) {
        u8* addr = (u8*)row + f.offset + (u64)k * esz;

        ImGui::PushID((int)(fi * 256u + k));
        // The label is always printed through ImGui::Text (its own internal vsnprintf, no
        // stdio.h needed in this file - src/editor/ has no such grant, tools/audit/includes.py);
        // the widget itself, when one is drawn, uses a hidden "##w" id and reads the printed
        // name from context, matching every other panel this lane has built.
        if (elems > 1u) { ImGui::Text("%s[%u]", f.name, k); } else { ImGui::Text("%s", f.name); }
        ImGui::SameLine();
        if (is_int_kind(f.kind)) {
            u64 tmp = 0;
            memcpy(&tmp, addr, esz);
            ImGui::InputScalar("##w", int_data_type(f.kind), &tmp);
            if (editable && ImGui::IsItemDeactivatedAfterEdit()) {
                (void)inspector_set_scalar_field(w, /*lockstep=*/false, sel, comp, fi, &tmp, esz);
            }
        } else if (f.kind == K_bool) {
            u8 tmp = 0;
            memcpy(&tmp, addr, esz);
            bool b = tmp != 0u;
            ImGui::Checkbox("##w", &b);
            if (editable && ImGui::IsItemDeactivatedAfterEdit()) {
                u8 nv = b ? 1u : 0u;
                (void)inspector_set_scalar_field(w, /*lockstep=*/false, sel, comp, fi, &nv, esz);
            }
        } else if (is_fx_kind(f.kind)) {
            i32 raw = 0;
            memcpy(&raw, addr, esz);
            const u8 frac = fx_frac_bits(f.kind);
            f64 shown = (f64)raw;
            for (u8 b = 0; b < frac; ++b) { shown *= 0.5; }
            ImGui::Text("%.9g (0x%08x)", shown, (u32)raw);
        } else if (is_handle_kind(f.kind)) {
            const u32 bits = load_handle_bits(addr, esz);
            if (bits == 0u) {
                ImGui::Text("%s null", handle_domain_name(f.kind));
            } else {
                u32 idx = 0, gen = 0;
                handle_idx_gen(bits, esz, &idx, &gen);
                ImGui::Text("%s #%u g%u", handle_domain_name(f.kind), idx, gen);
                if (f.kind == K_Entity) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("go")) {
                        Entity target{}; target.bits = bits;
                        ed->sel = target;
                    }
                }
            }
        } else if (f.kind == K_StrId) {
            u16 id = 0;
            memcpy(&id, addr, esz);
            if (w->interner != nullptr) {
                const StrView name = intern_name(w->interner, id);
                ImGui::TextUnformatted(name.ptr, name.ptr + name.len);
            } else {
                ImGui::Text("#%u", id);
            }
        } else {
            TL_FATAL("draw_field: unreachable FieldKind");
        }
        ImGui::PopID();
    }
}

}  // namespace

void inspector_panel_register(Editor* ed) { editor_register_panel(ed, "Inspector", inspector_panel_draw, true); }

ErrCode inspector_set_scalar_field(World* w, bool lockstep, Entity e, ComponentId comp,
                                    u32 field_index, const void* bytes, u32 len) {
    if (lockstep) { return ERR_EDITOR_LOCKSTEP; }
    world_set_field_cmd(w, e, comp, field_index, bytes, len);
    return ERR_OK;
}

void inspector_panel_draw(Editor* ed, World* w) {
    if (!ImGui::Begin("Inspector")) { ImGui::End(); return; }
    TL_CHECK(w != nullptr);

    for (u32 c = 0; c < w->comp_count; ++c) {
        const ComponentInfo* info = w->comps[c].info;
        if ((info->flags & COMP_HIDDEN) != 0u) { continue; }

        void* row = nullptr;
        Entity row_entity{};
        if ((info->flags & COMP_SINGLETON) != 0u) {
            row = w->comps[c].dense;
        } else {
            if (handle_is_null(ed->sel)) { continue; }
            row = column_get(&w->comps[c], ed->sel);
            if (row == nullptr) { continue; }
            row_entity = ed->sel;
        }

        ImGui::PushID((int)c);
        if (ImGui::CollapsingHeader(info->name)) {
            for (u32 fi = 0; fi < info->field_count; ++fi) {
                draw_field(ed, w, row_entity, (ComponentId)c, info, fi, row);
            }
        }
        ImGui::PopID();
    }
    // No custom_draw hook (ComponentInfo carries none) and no per-system debug_draw registry
    // exists (this file's Invariants note) - the generic walk above is the whole panel at v0.
    ImGui::End();
}
