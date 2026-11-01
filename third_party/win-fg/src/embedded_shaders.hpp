// framewarp: replaces win-fg's generated table. The SPIR-V is compiled from
// ../shaders at build time (CMakeLists.txt), one header per shader.
#pragma once
#include <cstddef>
#include <cstdint>

namespace winfg { namespace embedded {
#include "of3_luma_comp.h"
#include "of3_downsample_comp.h"
#include "of3_flow_comp.h"
#include "of3_expand_comp.h"
#include "of3_flow_m4_comp.h"
#include "of3_expand_m4_comp.h"
#include "of3_gm_reduce_comp.h"
#include "of3_gm_prewarp_comp.h"
#include "of3_flowreg_comp.h"
#include "wfg_synth_comp.h"

struct Blob { const uint32_t* code; size_t words; };
enum Shader { OF3_LUMA, OF3_DOWNSAMPLE, OF3_FLOW, OF3_EXPAND, OF3_FLOW_M4, OF3_EXPAND_M4, OF3_GM_REDUCE,
              OF3_GM_PREWARP, OF3_FLOWREG, WFG_SYNTH, SHADER_COUNT };
#define WFG_BLOB(a) Blob{a, sizeof(a) / sizeof(a[0])}
static inline Blob blob(Shader s) {
  switch (s) {
    case OF3_LUMA: return WFG_BLOB(of3_luma_comp);
    case OF3_DOWNSAMPLE: return WFG_BLOB(of3_downsample_comp);
    case OF3_FLOW: return WFG_BLOB(of3_flow_comp);
    case OF3_EXPAND: return WFG_BLOB(of3_expand_comp);
    case OF3_FLOW_M4: return WFG_BLOB(of3_flow_m4_comp);
    case OF3_EXPAND_M4: return WFG_BLOB(of3_expand_m4_comp);
    case OF3_GM_REDUCE: return WFG_BLOB(of3_gm_reduce_comp);
    case OF3_GM_PREWARP: return WFG_BLOB(of3_gm_prewarp_comp);
    case OF3_FLOWREG: return WFG_BLOB(of3_flowreg_comp);
    case WFG_SYNTH: return WFG_BLOB(wfg_synth_comp);
    default: return Blob{nullptr, 0};
  }
}
#undef WFG_BLOB
} } // namespace winfg::embedded
