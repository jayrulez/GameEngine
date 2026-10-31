// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// variants: HOLES
// preserve-interface
//
// Terrain depth-pass fragment stage, used only for holed chunks under HOLES (the camera prepass
// and the shadow cascades). It discards the fragments inside the cut by the bilinear hole mask,
// as terrain.ps does, so the prepass depth and the shadow map open where the surface does.
// Without the flag it is an empty stage that the pack cooks and nothing binds.
//
// Its vertex stage is the "terrain" VS module (the one the colour pass rasterizes with, so the
// prepass depth is bit-identical to the colour pass's), not terrain_depth.vs. So the input is
// terrain.vs's VSOut, member for member (the stage interface is matched by location, in
// declaration order; a shorter struct here is a Vulkan interface error, not a subset, and the
// preserve-interface directive keeps the unread members in the SPIR-V so the pairing is exact).
#ifdef HOLES
Texture2D    HoleMask    : register(t1, space2);
SamplerState HoleSampler : register(s0, space2);
struct VSOut {
    float4 pos      : SV_Position;
    float3 normal   : TEXCOORD0;
    float  heightT  : TEXCOORD1;
    float4 curClip  : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
    float3 worldPos : TEXCOORD4;
    float2 localXZ  : TEXCOORD5;
    float2 splatUV  : TEXCOORD6; // the hole mask's footprint UV, the only member read here
};
void main(VSOut i) {
    float2 dims;
    HoleMask.GetDimensions(dims.x, dims.y);
    if (HoleMask.Sample(HoleSampler, i.splatUV + 0.5 / max(dims, float2(1.0, 1.0))).r > 0.5) { discard; }
}
#else
void main(float4 pos : SV_Position) {}
#endif
