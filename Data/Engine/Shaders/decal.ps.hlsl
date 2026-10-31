// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#pragma pack_matrix(row_major)
Texture2D    SceneDepth : register(t0, space0);
SamplerState DepthSamp  : register(s0, space0);
Texture2D    DecalTex   : register(t0, space2);
SamplerState DecalSamp  : register(s0, space2);
cbuffer DecalUniforms : register(b0, space1) {
    row_major float4x4 World;
    row_major float4x4 InvWorld;
    row_major float4x4 InvViewProj;
    float4   Color;
    float2   InvTargetSize; // 1 / the full target's size
    float    FadeStartCos;  // cos(the angle fade's start)
    float    FadeEndCos;    // cos(the angle fade's end)
    float    YFlip;         // interpolant Y correction: +1 Vulkan (neg viewport), -1 Y-flip targets
    float    _decalPad0;
    float    _decalPad1;
    float    _decalPad2;
};

float4 main(float4 pos : SV_Position, float2 ndc : TEXCOORD0) : SV_Target {
    // Sample the scene depth at this framebuffer pixel. SV_Position is the framebuffer position, so
    // pixel/size reads the texel the forward pass wrote here; a same-pixel read needs no flip.
    float2 uv    = pos.xy * InvTargetSize;
    float  depth = SceneDepth.SampleLevel(DepthSamp, uv, 0).r;

    // Reconstruct the world position from the interpolated emitted NDC. That NDC is viewport-local,
    // which is what the per-view InvViewProj expects (a full-target-uv reconstruction breaks
    // split-screen sub-rect views). On Y-flip targets the interpolant's pixel mapping mirrors, so
    // YFlip corrects its Y (the sky's SceneYSign convention: +1 on Vulkan keeps it as is, -1 on
    // Y-flip targets un-mirrors it). Clip goes to world through InvViewProj, as in SkyPass.
    ndc.y *= YFlip;
    float4 h        = mul(float4(ndc, depth, 1.0), InvViewProj);
    float3 worldPos = h.xyz / h.w;

    // Into the decal's unit box; clip outside [-0.5, 0.5].
    float3 local = mul(float4(worldPos, 1.0), InvWorld).xyz;
    if (any(abs(local) > 0.5)) { discard; }

    // Decal UV from the box's local XY (project along local Z). Flip V for top-origin texture space.
    float2 decalUV = float2(local.x + 0.5, 0.5 - local.y);

    // Angle fade: the receiver normal (from the world position's screen derivatives) against the
    // decal's projection axis (local +Z in world, World row 2). Fade out where the surface faces away
    // from the projection.
    float3 N        = normalize(cross(ddy(worldPos), ddx(worldPos)));
    float3 decalFwd = normalize(float3(World._m20, World._m21, World._m22));
    float  cosA     = dot(N, -decalFwd);
    float  fade     = smoothstep(FadeEndCos, FadeStartCos, cosA);

    float4 c = DecalTex.Sample(DecalSamp, decalUV) * Color;
    c.a *= fade;
    return c;
}
