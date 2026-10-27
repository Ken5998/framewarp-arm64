#version 450

layout(set = 0, binding = 0) uniform sampler2D last_frame;

layout(push_constant) uniform Params {
    vec4 tint;   // rgb, a = strength (0 = off)
    vec2 shift;  // pixels
    int taps;    // 1 = plain copy, 9 = extra reads to mimic a warp pass
} pc;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main()
{
    vec2 px = 1.0 / vec2(textureSize(last_frame, 0));
    vec2 p = uv - pc.shift * px;
    vec4 c = texture(last_frame, p);
    if (pc.taps > 1) {
        // Neighbour reads with a tiny weight: the cost of a 3x3 warp
        // footprint without blurring the picture.
        vec4 n = vec4(0.0);
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++)
                n += texture(last_frame, p + vec2(x, y) * px);
        c += n * 1e-4;
    }
    color = vec4(mix(c.rgb, pc.tint.rgb, pc.tint.a), 1.0);
}
