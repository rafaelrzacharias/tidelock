# FixPointCS (vendored, tools only)

Source: https://github.com/XMunkki/FixPointCS, commit `a852f05b428a942f8dc274ee516a893ae224e0d4`
(master, fetched 2026-08-23). MIT - `LICENSE.txt` (c) Jere Sanisalo, Petri Kero.

Files: `Cpp/FixedUtil.h`, `Cpp/Fixed32.h`, `Cpp/Fixed64.h`, verbatim. Used ONLY by
`tools/fxcheck` as the differential reference of `docs/FX-PALETTE.md` §4.4 layer 2; nothing under
`src/` includes it (the include firewall would reject it). The kernels in
`src/foundation/det_math.cpp` are *ports* of `Fixed32::UnitSin` (`FixedUtil::SinPoly4`) and
`FixedUtil::AtanPoly5Lut8`; the coefficient integers there are copied from these files (atan's
pre-scaled to turns by `tools/fxcheck/oracle.py`), with the attribution required by the licence.
