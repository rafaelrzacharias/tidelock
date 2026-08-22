# Alloy - the matter sim (docs/ALLOY.md). Depends on foundation_det ONLY
# (docs/ARCHITECTURE.md §1 rule 3); symbol-audited, tl_flags_sim.
tl_module(tl_sim SIM)
target_link_libraries(tl_sim PUBLIC tl_foundation_det)
