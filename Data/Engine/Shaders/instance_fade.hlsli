// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// INSTANCE FADE: a per-instance distance dissolve for instanced sets (vegetation), applied in the
// forward, shadow-depth and pick vertex shaders under INSTANCED. The set's window (start, end
// in metres; end <= 0 = no fade) rides the pass's view block, one private slot per faded set;
// each instance's RANK (its position in the set's random order, (i + 0.5) / count) rides its
// Tint.a. An instance whose rank is above the density at ITS OWN distance collapses to its
// origin (zero area, nothing rasterised) after shrinking over the last eighth of its window, so
// a set dissolves instance by instance with no seam between chunks. The CPU draw-count prefix
// (the chunk's nearest distance) is exactly the ranks a per-instance test could still keep, so
// the two agree. The density curve is Foundation's DensityAtDistance: 1 inside start, a
// smoothstep to 0 at end.
float InstanceFadeKeep(float3 instanceOrigin, float3 cameraPos, float rank, float2 fade) {
    if (fade.y <= 0.0) return 1.0;
    float d = distance(cameraPos, instanceOrigin);
    float t = saturate((d - fade.x) / max(fade.y - fade.x, 1e-3));
    float density = 1.0 - t * t * (3.0 - 2.0 * t);
    return saturate((density - rank) * 8.0);
}
