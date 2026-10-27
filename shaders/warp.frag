#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

// A generated frame: the last real frame pushed forward along its motion.
// A pixel at x, phase t real frames later, comes from x - t * motion(x),
// except where that would smear:
// - pixels that did not change between the last two real frames (the HUD,
//   overlays) stay where they are;
// - pixels whose source is such a static pixel, or off the screen, keep the
//   last real frame's pixel instead of dragging the HUD or the edge along.
// In camera mode the motion is one smooth model for the whole frame, except
// where the analysis found nothing moved (the player's car, the HUD).

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

const float kStatic = 3.0 / 255.0;

bool unchanged(vec2 at)
{
    vec3 d = abs(texture(frame, at).rgb - texture(frame_prev, at).rgb);
    return max(d.r, max(d.g, d.b)) < kStatic;
}

void main()
{
    vec2 px = 1.0 / vec2(textureSize(frame, 0));
    vec3 here = texture(frame, uv).rgb;
    vec2 m = vec2(0.0);
    bool still = false;
    if (camera_mode() && pc.has_prev != 0) {
        vec2 cam = camera_motion(gl_FragCoord.xy, 1.0 / px);
        ivec2 size = textureSize(pixel_motion, 0);
        ivec2 base = ivec2(floor(gl_FragCoord.xy / float(kScale) - 0.5));
        float moving = 0.0;
        for (int y = 0; y <= 1; y++)
            for (int x = 0; x <= 1; x++)
                moving += texelFetch(pixel_motion, clamp(base + ivec2(x, y), ivec2(0), size - 1), 0).w;
        if (moving == 4.0) {
            m = cam;
        } else if (moving > 0.0) {
            // On the edge between a still part and the rest, per pixel.
            vec3 dc = abs(here - texture(frame_prev, uv - cam * px).rgb);
            vec3 dz = abs(here - texture(frame_prev, uv).rgb);
            if (dc.r + dc.g + dc.b < dz.r + dz.g + dz.b)
                m = cam;
        }
        still = m == vec2(0.0);
    } else if (pc.mode != 0 && pc.has_prev != 0) {
        // The motion is known per 4x4 pixels; each pixel takes, of the 2x2
        // nearest of those, the vector that best explains its own colour in
        // the previous frame, so edges stay sharp instead of 4x4 steps.
        ivec2 size = textureSize(pixel_motion, 0);
        ivec2 base = ivec2(floor(gl_FragCoord.xy / float(kScale) - 0.5));
        float best = 1e9;
        for (int y = 0; y <= 1; y++)
            for (int x = 0; x <= 1; x++) {
                vec2 cand = texelFetch(pixel_motion, clamp(base + ivec2(x, y), ivec2(0), size - 1), 0).xy;
                vec3 d = abs(here - texture(frame_prev, uv - cand * px).rgb);
                float cost = d.r + d.g + d.b;
                if (cost < best) {
                    best = cost;
                    m = cand;
                }
            }
    }
    vec3 c = here;
    if (dot(m, m) > 0.0 && !unchanged(uv)) {
        vec2 src = uv - pc.phase * m * px;
        // The camera warp moves the whole frame, so near the edges it takes
        // the edge pixel rather than leaving a seam.
        bool inside = camera_mode() ||
                      (all(greaterThanEqual(src, vec2(0.0))) && all(lessThanEqual(src, vec2(1.0))));
        if (inside && !unchanged(src))
            c = texture(frame, src).rgb;
    }
    if (pc.mode == 2) {
        // Motion overlay: hue = direction, brightness = speed.
        float speed = clamp(length(m) / 64.0, 0.0, 1.0);
        vec3 dir = 0.5 + 0.5 * vec3(normalize(m + 1e-6), -normalize(m + 1e-6).x);
        c = mix(c, dir, speed * 0.7);
    } else if (pc.mode == 4 && pc.phase > 0.0) {
        // Camera overlay on the generated frames: blue = kept still (should
        // be only the car and the HUD), yellow = the camera moves this pixel
        // less than 2 pixels per real frame.
        if (still)
            c = mix(c, vec3(0.0, 0.3, 1.0), 0.5);
        else if (dot(m, m) < 4.0)
            c = mix(c, vec3(1.0, 0.9, 0.0), 0.5);
    }
    color = vec4(mix(c, pc.tint.rgb, pc.tint.a), 1.0);
}
