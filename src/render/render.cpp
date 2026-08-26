// Placeholder TU for tl_render - a static lib needs one source (docs/LESSONS.md: "every empty
// module folder carries one placeholder .cpp until its lane lands"). Doubles as the header-first
// commit's smoke-compile target: every public header this lane owns is included here so a
// syntax/include error surfaces at build time, not at the first real .cpp. Deleted once the real
// implementation .cpp files (camera.cpp, queue.cpp, batch.cpp, sprite.cpp, extract.cpp,
// backend_sdl.cpp, simview.cpp, debugdraw.cpp, text.cpp) exist and pull the same headers in.
#include "render/render.h"
#include "render/camera.h"
#include "render/queue.h"
#include "render/sprite.h"
#include "render/simview.h"
#include "render/debugdraw.h"
#include "render/text.h"

extern const u32 tl_render_unit;
const u32 tl_render_unit = 0;
