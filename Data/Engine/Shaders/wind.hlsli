// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
//
// WIND: a vertex-shader sway for vegetation cards and tufts, included by the forward, shadow-depth
// and pick vertex shaders under the WIND variant (each stage declares WIND in its variants line).
// The sway is a world-space offset, sin(time * WindSpeed + hash(world XZ)) * WindStrength scaled by
// the vertex's height mask (local y over WindHeight, squared: roots stay put, tips move). The
// three parameters are MATERIAL properties (the forward set-2 cbuffer's spare lanes), so a card
// material opts in and a rock never sways; the renderer selects this variant only for a material
// whose WindStrength default is above zero, so every other material is byte-identical to before.
// The same offset is applied to the previous-frame position at the previous time, so motion
// vectors follow the sway.

cbuffer Material : register(b0, space2) {
    float4 BaseColor;
    float  Metallic;
    float  Roughness;
    float  WindStrength;       // metres of sway at full height mask (0 = the variant is never chosen)
    float  WindSpeed;          // radians per second
    float4 EmissiveColor;
    float  OcclusionStrength;
    float  NormalScale;
    float  AlphaCutoff;
    float  WindHeight;         // local y at which the sway reaches full strength (roots at y <= 0)
};

#include "wind_sway.hlsli"

float3 WindSway(float3 worldPos, float localY, float time) {
    return WindSwayOf(worldPos, localY, time, WindStrength, WindSpeed, WindHeight);
}
