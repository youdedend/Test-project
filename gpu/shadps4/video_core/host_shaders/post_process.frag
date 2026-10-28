// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    // uint, not bool: GLSL bools are 4 bytes and the host struct is 4 bytes each, so this is the
    // only spelling where the two layouts are the same layout.
    uint hdr;
    uint srgb_input;
    float bloom;
} pp;

// Bloom reduction (bbport.ini `bloom=`, in-game menu "Bloom (highlight rolloff)").
//
// The game's bloom has already been composited by the time this pass sees the frame, so it cannot be
// removed here. What this does is compress the highlights that the bloom leaves behind: everything
// above `knee` is rolled off toward 1.0 instead of clipping there, so a lantern lights its
// surroundings without turning its own core into a white blob. It is a one-line op on a value the
// pass already has its hands on - no extra target, no extra bandwidth, no measurable cost.
//
// At pp.bloom == 0 the branch is not taken and the output is bit-identical to before.
//
// The curve is the usual exponential shoulder: continuous at the knee, slope 1 just below it (so the
// mid-tones are untouched) and asymptotic to 1.0 instead of clipping.
const float bloom_knee = 0.72;
vec3 rolloff_highlights(vec3 rgb) {
    vec3 over = max(rgb - vec3(bloom_knee), vec3(0.0));
    vec3 rolled = vec3(bloom_knee) +
                  vec3(1.0 - bloom_knee) * (vec3(1.0) - exp(-over / (1.0 - bloom_knee)));
    return mix(rgb, rolled, step(vec3(bloom_knee), rgb));
}

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr != 0u) {
        // HDR output is already in the display's own space and is graded by the TV: the rolloff is
        // not applied there, so `bloom=` has no effect while HDR output is on. The menu says so.
        color = color_linear;
    } else {
        if (pp.srgb_input != 0u) color_linear.rgb = degamma(color_linear.rgb);
        if (pp.bloom > 0.0) {
            // In linear light, before the transfer curve: compressing after gamma would crush the
            // shadows along with the highlights.
            color_linear.rgb = mix(color_linear.rgb, rolloff_highlights(color_linear.rgb), pp.bloom);
        }
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
