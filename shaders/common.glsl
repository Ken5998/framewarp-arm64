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
layout(set = 0, binding = 10) uniform sampler2D frame_prev; // the real frame before (linear)
layout(set = 0, binding = 11, rgba16f) uniform writeonly image2D pixel_motion_out;
layout(set = 0, binding = 12) uniform sampler2D pixel_motion; // per luma pixel
layout(set = 0, binding = 13, rgba32f) uniform writeonly image2D camera_out;
layout(set = 0, binding = 14) uniform sampler2D camera;   // 4x1: the global motion model

layout(push_constant) uniform Params {
    vec4 tint;      // rgb, a = strength (0 = off)
    float phase;    // how far ahead of the real frame, in real frames
    int mode;       // 0 = copy, 1 = warp, 2 = warp + motion overlay, 3 = camera,
                    // 4 = camera + overlay
    int has_prev;   // luma_prev holds a real frame
    float zero_bias;
} pc;

const int kScale = 4;  // luma is 1/4 of the frame size, luma2 1/4 of luma
const int kBlock = 8;  // motion block, in luma pixels (32 frame pixels)

bool camera_mode()
{
    return pc.mode >= 3;
}

// The camera model, in frame pixels per real frame, with p in frame heights
// from the centre: motion.x = cx . (1, x, y, x^2, xy, y^2), likewise y.
// camera texels: 0 = cx[0..3], 1 = (cx[4], cx[5], share of the moving blocks
// that agree with it, share of the blocks that move), 2 = cy[0..3],
// 3 = (cy[4], cy[5]); the shares are only for the log.
vec2 camera_motion(vec2 frag, vec2 size)
{
    vec2 p = (frag - 0.5 * size) / size.y;
    vec4 f0 = vec4(1.0, p.x, p.y, p.x * p.x);
    vec2 f1 = vec2(p.x * p.y, p.y * p.y);
    vec4 x0 = texelFetch(camera, ivec2(0, 0), 0), x1 = texelFetch(camera, ivec2(1, 0), 0);
    vec4 y0 = texelFetch(camera, ivec2(2, 0), 0), y1 = texelFetch(camera, ivec2(3, 0), 0);
    vec2 m = vec2(dot(x0, f0) + dot(x1.xy, f1), dot(y0, f0) + dot(y1.xy, f1));
    // Far from the blocks it was fitted to a quadratic can run away; no
    // more than the block search could have found.
    return m * min(1.0, 160.0 / max(length(m), 1e-6));
}
