// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Vegetation - :pick implementation.

module;
#include "Core/Prelude.h"

module editor.vegetation;

import foundation.core;
import foundation.scene;
import foundation.heightfield;
import foundation.terrain.resource;
import foundation.vegetation.resource;
import engine.terrain;
import engine.vegetation;

using namespace foundation::core;

namespace editor
{
    namespace scene = foundation::scene;
    namespace hf = foundation::heightfield;
    using engine::vegetation::TerrainVegetationComponent;
    using engine::vegetation::TerrainVegetationComponentManager;

    namespace
    {
        [[nodiscard]] TerrainVegetationComponentManager* VegetationManagerOf(scene::Scene& scene)
        {
            scene::ComponentManagerBase* base =
                scene.FindManagerByComponentType(TypeOf<TerrainVegetationComponent>());
            return static_cast<TerrainVegetationComponentManager*>(base);
        }

        // The heightfield under a vegetation component: its entity's terrain, or else the nearest
        // ancestor's, the same rule the manager uses.
        [[nodiscard]] hf::Heightfield* HeightfieldFor(scene::Scene& scene, scene::EntityHandle owner,
                                                      scene::EntityHandle& terrainEntity)
        {
            auto* terrains = scene.GetSystem<engine::terrain::TerrainComponentManager>();
            if (terrains == nullptr)
            {
                return nullptr;
            }
            scene::EntityHandle e = owner;
            for (u32 depth = 0; depth < 64 && e.IsAssigned(); ++depth)
            {
                if (engine::terrain::TerrainComponent* tc = terrains->Get(e))
                {
                    foundation::terrain::TerrainResource* res = tc->terrain.Get();
                    hf::Heightfield* grid = res != nullptr ? res->heightfield.Get() : nullptr;
                    if (grid != nullptr && !grid->IsEmpty())
                    {
                        terrainEntity = e;
                        return grid;
                    }
                    return nullptr;
                }
                e = scene.GetParent(e);
            }
            return nullptr;
        }
    }

    bool VegetationPick::AnyFootprint(scene::Scene& scene, bool requireMask)
    {
        TerrainVegetationComponentManager* mgr = VegetationManagerOf(scene);
        if (mgr == nullptr)
        {
            return false;
        }
        bool any = false;
        mgr->ForEach(
            [&](TerrainVegetationComponent& c, scene::EntityHandle owner)
            {
                if (any)
                {
                    return;
                }
                if (requireMask && (c.mask.Get() == nullptr || c.mask.Get()->IsEmpty()))
                {
                    return;
                }
                scene::EntityHandle terrainEntity{};
                any = HeightfieldFor(scene, owner, terrainEntity) != nullptr;
            });
        return any;
    }

    VegetationPick VegetationPick::Resolve(scene::Scene& scene, Float3 rayOrigin, Float3 rayDirection,
                                       bool requireMask, bool ignoreHoles)
    {
        VegetationPick best;
        TerrainVegetationComponentManager* mgr = VegetationManagerOf(scene);
        if (mgr == nullptr)
        {
            return best;
        }
        f32 bestDist = kFloatMax;
        mgr->ForEach(
            [&](TerrainVegetationComponent& c, scene::EntityHandle owner)
            {
                foundation::vegetation::VegetationMask* mask = c.mask.Get();
                if (requireMask && (mask == nullptr || mask->IsEmpty()))
                {
                    return;
                }
                scene::EntityHandle terrainEntity{};
                hf::Heightfield* grid = HeightfieldFor(scene, owner, terrainEntity);
                if (grid == nullptr)
                {
                    return;
                }
                const Float4x4 world = scene.GetWorldMatrix(terrainEntity);
                const Float4x4 inv = Inverse(world);
                const Float3 localOrigin = TransformPoint(rayOrigin, inv);
                const Float3 localDir = TransformDirection(rayDirection, inv);
                f32 t = 0.0f;
                const bool hit = ignoreHoles ? grid->QueryRayIgnoringHoles(localOrigin, localDir, t)
                                             : grid->QueryRay(localOrigin, localDir, t);
                if (!hit)
                {
                    return;
                }
                const Float3 localHit = localOrigin + Normalized(localDir) * t;
                const Float3 worldHit = TransformPoint(localHit, world);
                const f32 dist = Length(worldHit - rayOrigin);
                if (dist >= bestDist)
                {
                    return;
                }
                const Float2 ws = grid->WorldSize();
                bestDist = dist;
                best.component = &c;
                best.manager = mgr;
                best.owner = owner;
                best.terrainEntity = terrainEntity;
                best.heightfield = grid;
                best.mask = (mask != nullptr && !mask->IsEmpty()) ? mask : nullptr;
                best.maskId = c.mask.id;
                best.gridSize = grid->Size();
                best.worldSizeX = ws.x != 0.0f ? ws.x : 1.0f;
                best.worldSizeY = ws.y != 0.0f ? ws.y : 1.0f;
                best.uvX = localHit.x / best.worldSizeX + 0.5f;
                best.uvY = localHit.z / best.worldSizeY + 0.5f;
                best.localHit = localHit;
                best.worldHit = worldHit;
                best.worldNormal =
                    Normalized(TransformDirection(grid->GetNormalAt(localHit.x, localHit.z), world));
                best.terrainWorld = world;
                best.valid = true;
            });
        return best;
    }
}
