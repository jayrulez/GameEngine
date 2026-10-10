// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Terrain - reflection + scene-composition implementation unit.
//
// The REFLECT_VALUE body lives here (kept out of the interface; see gcc-module-interface-hygiene).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.terrain;
import engine.domain;
import foundation.heightfield;
import foundation.heightfield.resource;

import foundation.core;
import foundation.scene;
import foundation.terrain.resource;

using namespace foundation::core;

namespace engine::terrain
{
    void TerrainComponentManager::CollectStaticGeometry(scene::Scene& scene, const AABB& bounds, f32 detail,
                                                        Array<Float3>& outTriangles)
    {
        ForEach(
            [&](TerrainComponent& c, scene::EntityHandle entity)
            {
                foundation::terrain::TerrainResource* terrain = c.terrain.Get();
                foundation::heightfield::Heightfield* field =
                    (terrain != nullptr) ? terrain->heightfield.Get() : nullptr;
                if (field == nullptr || field->Size() < 2 || !scene.IsEffectivelyActive(entity))
                {
                    return;
                }
                const Float4x4 terrainWorld = scene.GetWorldMatrix(entity);
                const Float2 footprint = field->WorldSize();
                const AABB localBox{Float3{-footprint.x * 0.5f, field->MinY(), -footprint.y * 0.5f},
                                    Float3{footprint.x * 0.5f, field->MaxY(), footprint.y * 0.5f}};
                if (!TransformAABB(localBox, terrainWorld).Intersects(bounds))
                {
                    return;
                }

                // The box in terrain-local space bounds the grid range to triangulate.
                const AABB boundsLocal = TransformAABB(bounds, Inverse(terrainWorld));
                const i32 last = field->Size() - 1;
                const Float2 g0 = field->WorldToGrid(boundsLocal.min.x, boundsLocal.min.z);
                const Float2 g1 = field->WorldToGrid(boundsLocal.max.x, boundsLocal.max.z);
                const i32 x0 = Clamp(static_cast<i32>(Floor(g0.x)), 0, last);
                const i32 z0 = Clamp(static_cast<i32>(Floor(g0.y)), 0, last);
                const i32 x1 = Clamp(static_cast<i32>(Ceil(g1.x)), 0, last);
                const i32 z1 = Clamp(static_cast<i32>(Ceil(g1.y)), 0, last);
                if (x1 <= x0 || z1 <= z0)
                {
                    return;
                }

                // Samples no finer than `detail` (Recast re-voxelizes anyway); the last row and
                // column are always included so the surface reaches the edge of the range.
                const f32 spacing = footprint.x / static_cast<f32>(last);
                const i32 stride = Max(1, static_cast<i32>(detail / Max(spacing, 0.0001f)));
                Array<i32> xs;
                Array<i32> zs;
                for (i32 gx = x0; gx < x1; gx += stride)
                {
                    xs.PushBack(gx);
                }
                xs.PushBack(x1);
                for (i32 gz = z0; gz < z1; gz += stride)
                {
                    zs.PushBack(gz);
                }
                zs.PushBack(z1);

                const auto at = [&](i32 gx, i32 gz)
                {
                    const Float2 xz = field->GridToWorld(static_cast<f32>(gx), static_cast<f32>(gz));
                    return TransformPoint(Float3{xz.x, field->GetHeightAtGrid(gx, gz), xz.y}, terrainWorld);
                };
                for (usize row = 0; row + 1 < zs.Size(); ++row)
                {
                    for (usize col = 0; col + 1 < xs.Size(); ++col)
                    {
                        // A hole anywhere in this block (the stride square, interior included) is
                        // no surface: its two triangles are left out, so the navmesh opens there.
                        if (field->BlockHasHole(xs[col], zs[row], xs[col + 1], zs[row + 1]))
                        {
                            continue;
                        }
                        const Float3 v00 = at(xs[col], zs[row]);
                        const Float3 v10 = at(xs[col + 1], zs[row]);
                        const Float3 v01 = at(xs[col], zs[row + 1]);
                        const Float3 v11 = at(xs[col + 1], zs[row + 1]);
                        // Counter-clockwise seen from above: the faces point up.
                        const Float3 quad[] = {v00, v01, v11, v00, v11, v10};
                        for (const Float3& p : quad)
                        {
                            outTriangles.PushBack(p);
                        }
                    }
                }
            });
    }
}

namespace engine::terrain
{
    REFLECT_VALUE(TerrainComponent, "rtti::engine::terrain")
    {
        builder.Attribute("displayName", String(u8"Terrain"))
            .Attribute("category", String(u8"Terrain"))
            .Attribute("description",
                       String(u8"Draws a terrain asset, its heights and painted layers, as "
                              u8"ground that navigation can walk."))
            .DataVersion(2)
            .Property<&TerrainComponent::terrain>("terrain")
            .Property<&TerrainComponent::castShadows>("castShadows")
            .Property<&TerrainComponent::visible>("visible")
            .Property<&TerrainComponent::lodBias>("lodBias");
    }

    void AddTerrainSceneManagers(foundation::scene::Scene& scene)
    {
        scene.AddSystem<TerrainComponentManager>();
    }

    void RegisterTerrainComponentReflection()
    {
        static const bool once = []()
        {
            RttiRegisterValue_TerrainComponent();
            return true;
        }();
        (void)once;
    }
}

namespace engine::terrain
{
    const engine::DomainModule& TerrainDomain() noexcept
    {
        static const foundation::resource::ResourceModule* const kResources[] = {
            &foundation::terrain::kTerrainResourceModule,
            &foundation::heightfield::kHeightfieldResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"terrain",
            .installScene = &AddTerrainSceneManagers,
            .registerReflection = &RegisterTerrainComponentReflection,
            .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{
                kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}
