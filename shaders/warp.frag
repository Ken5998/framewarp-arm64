#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// A generated frame: the last real frame pushed forward along its motion.
// A pixel at x, phase t real frames later, comes from x - t * motion(x).

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main()
{
    vec2 px = 1.0 / vec2(textureSize(frame, 0));
    // The motion texture covers whole 32 pixel blocks, a bit more than the frame.
    vec2 muv = uv * vec2(textureSize(frame, 0)) / vec2(textureSize(motion, 0) * kScale * kBlock);
    vec2 m = pc.mode == 0 ? vec2(0.0) : texture(motion, muv).xy;
    vec3 c = texture(frame, uv - pc.phase * m * px).rgb;
    if (pc.mode == 2) {
        // Motion overlay: hue = direction, brightness = speed.
        float speed = clamp(length(m) / 64.0, 0.0, 1.0);
        vec3 dir = 0.5 + 0.5 * vec3(normalize(m + 1e-6), -normalize(m + 1e-6).x);
        c = mix(c, dir, speed * 0.7);
    }
    color = vec4(mix(c, pc.tint.rgb, pc.tint.a), 1.0);
}
