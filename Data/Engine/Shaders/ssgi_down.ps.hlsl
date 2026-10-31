// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SSGI radiance prefilter: a quarter-res box downsample of the lit HDR, which is the trace's
// gather source. This is the main defence against fireflies: every radiance tap the trace
// takes is already an average of about 16 pixels, so a single blown-out texel is diluted 16:1
// before any ray sees it. Variance is removed at the source instead of asking the denoisers to
// fix a spiked estimate after the fact.

#include "push_constant.hlsli"
Texture2D<float4> SceneTex  : register(t0, space0);
SamplerState      LinearSamp: register(s0, space0);

struct SsgiDownPush {
    float2 SrcTexelSize;   // 1 / source (full-res) size
    float2 _pad0;
};
PUSH_CONSTANT(SsgiDownPush, pc, space1);

float3 Tap(float2 uv) {
    float3 c = SceneTex.SampleLevel(LinearSamp, uv, 0).rgb;
    // Sanitize per tap: one inf or NaN texel must not poison the average, and a NaN cannot
    // be repaired anywhere downstream. No isnan, because naga cannot translate OpIsNan (WGSL
    // removed isNan). A NaN fails c == c instead (select becomes OpSelect, which naga
    // handles), and clamp's min and max remove the infinities.
    return select(c == c, clamp(c, 0.0, 65504.0), float3(0.0, 0.0, 0.0));
}

// Four bilinear taps on the 2x2-quad corners make a 4x4 box of the source per output texel.
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 st = pc.SrcTexelSize;
    float3 c = Tap(uv + float2(-1.0, -1.0) * st) + Tap(uv + float2(1.0, -1.0) * st) +
               Tap(uv + float2(-1.0, 1.0) * st) + Tap(uv + float2(1.0, 1.0) * st);
    return float4(c * 0.25, 1.0);
}
