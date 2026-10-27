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
layout(set = 0, binding = 14) uniform sampler2D camera;   // 2x1: the global motion model

layout(push_constant) uniform Params {
    vec4 tint;      // rgb, a = strength (0 = off)
    float phase;    // how far ahead of the real frame, in real frames
    int mode;       // 0 = copy, 1 = warp, 2 = warp + motion overlay, 3 = camera
    int has_prev;   // luma_prev holds a real frame
    float zero_bias;
} pc;

const int kScale = 4;  // luma is 1/4 of the frame size, luma2 1/4 of luma
const int kBlock = 8;  // motion block, in luma pixels (32 frame pixels)

// The camera model: motion(p) = A p + b in frame pixels per real frame, p in
// frame heights from the centre. camera texel 0 = (a11, a12, b.x),
// texel 1 = (a21, a22, b.y).
vec2 camera_motion(vec2 frag, vec2 size)
{
    vec2 p = (frag - 0.5 * size) / size.y;
    vec4 r0 = texelFetch(camera, ivec2(0, 0), 0);
    vec4 r1 = texelFetch(camera, ivec2(1, 0), 0);
    return vec2(dot(r0.xyz, vec3(p, 1.0)), dot(r1.xyz, vec3(p, 1.0)));
}
