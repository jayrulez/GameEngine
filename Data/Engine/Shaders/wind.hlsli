// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
//
// WIND: a vertex-shader sway for vegetation cards and tufts, included by the forward, shadow-depth
// and pick vertex stages under the WIND variant (each stage declares WIND in its variants line).
// The sway is a world-space offset, sin(time * Speed + hash(world XZ)) * Strength scaled by the
// vertex's height mask (local y over Height, squared: roots stay put, tips move). The parameters
// are the effect's own (Specs/wind-for-every-shader.md): the block below becomes
// `ConstantBuffer<WindEffect> Wind : register(b1, space2)`, read as `Wind.Strength`, and a material
// whose shader declares WIND carries them as its Wind group, which the material system binds at
// that binding whatever the material's own layout is. The renderer selects this variant only for
// a material whose gate (Strength) is above zero, so every other material is byte-identical to
// before. The same offset is applied to the previous-frame position at the previous time, so
// motion vectors follow the sway.

effect Wind : WIND
{
    float Strength = 0 [0, 2] (gate);   // metres of sway at full height mask (0 = off)
    float Speed = 1 [0, 20];            // radians per second
    float Height = 1 [0.01, 100];       // local y at which the sway reaches full strength
}

#include "wind_sway.hlsli"

float3 WindSway(float3 worldPos, float localY, float time) {
    return WindSwayOf(worldPos, localY, time, Wind.Strength, Wind.Speed, Wind.Height);
}
