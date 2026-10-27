// Shared by every pass: one descriptor set, two copies (one per history
// slot), and one push constant block.

layout(set = 0, binding = 0) uniform sampler2D frame;     // last real frame (linear)
layout(set = 0, binding = 1) uniform sampler2D luma_cur;  // its luma, 1/4 size
layout(set = 0, binding = 2) uniform sampler2D luma_prev; // the frame before
layout(set = 0, binding = 3, r32f) uniform writeonly image2D luma_out;
layout(set = 0, binding = 4, rgba16f) uniform image2D motion_raw;
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D motion_out;
layout(set = 0, binding = 6) uniform sampler2D motion;    // filtered (linear)
layout(set = 0, binding = 7) uniform sampler2D luma2_cur;  // luma at 1/16 size
layout(set = 0, binding = 8) uniform sampler2D luma2_prev;
layout(set = 0, binding = 9, r32f) uniform writeonly image2D luma2_out;

layout(push_constant) uniform Params {
    vec4 tint;      // rgb, a = strength (0 = off)
    float phase;    // how far ahead of the real frame, in real frames
    int mode;       // 0 = copy, 1 = warp, 2 = warp + motion overlay
    int has_prev;   // luma_prev holds a real frame
    float zero_bias;
} pc;

const int kScale = 4;  // luma is 1/4 of the frame size, luma2 1/4 of luma
const int kBlock = 8;  // motion block, in luma pixels (32 frame pixels)
