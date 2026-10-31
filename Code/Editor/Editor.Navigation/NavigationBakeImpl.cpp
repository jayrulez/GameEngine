// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Navigation - implementation unit.
//
// The bake logic and its heavy imports (navigation.pipeline for the asset write) live here, out of
// the interface, to keep GCC's module build light. See NavigationBake.cppm for the surface.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.navigation;

import foundation.core;
import foundation.scene;
import foundation.content;
import engine.navigation;
import foundation.navigation;
import foundation.navigation.resource;
import navigation.pipeline;
import editor.core;
import foundation.settings;

using namespace foundation::core;

namespace editor::navigation
{
    namespace nav = foundation::navigation;

    namespace
    {
        // A zone's bake settings. The bake covers the zone's box (zone-local, where the geometry
        // is), so a ground plane as wide as the level does not widen the grid past the zone.
        [[nodiscard]] nav::NavigationBakeParams ParamsFor(const engine::navigation::NavMeshZoneComponent& zone,
                                                          bool parallelBake)
        {
            nav::NavigationBakeParams params;
            params.cellSize = zone.cellSize;
            params.cellHeight = zone.cellHeight;
            params.agentRadius = zone.agentRadius;
            params.agentHeight = zone.agentHeight;
            params.agentMaxClimb = zone.agentMaxClimb;
            params.agentMaxSlopeDegrees = zone.agentMaxSlopeDegrees;
            params.parallelBake = parallelBake; // the domain-contributed editor setting
            params.bounds = AABB{-zone.extents, zone.extents};
            return params;
        }
    }

    void RegisterNavigationEditorSettingsTypes()
    {
        GlobalTypeRegistry().Register(NavigationEditorSettings::StaticType());
        RegisterSerializable<NavigationEditorSettings>();
    }

    void RegisterNavigationEditorSettings(editor::EditorContext& context)
    {
        RegisterNavigationEditorSettingsTypes(); // idempotent; covers boot orders that skipped it

        editor::EditorContext* ctx = &context;
        editor::EditorContext::EditorSettingsContribution contribution;
        contribution.category = String(u8"Navigation");
        editor::EditorContext::EditorSettingsBoolField parallel;
        parallel.label = String(u8"Parallel navmesh bake");
        parallel.description = String(
            u8"Bake navmesh tiles across worker threads. The output is byte-identical either "
            u8"way - this only trades bake latency.");
        parallel.get = [ctx]() { return ParallelBakeEnabled(*ctx); };
        parallel.set = [ctx](bool value)
        {
            if (foundation::settings::Settings* store = ctx->UserEditorSettings())
            {
                store->Section<NavigationEditorSettings>().parallelBake = value;
                store->MarkChanged<NavigationEditorSettings>();
            }
        };
        contribution.bools.PushBack(
            static_cast<editor::EditorContext::EditorSettingsBoolField&&>(parallel));
        context.RegisterEditorSettingsContribution(
            static_cast<editor::EditorContext::EditorSettingsContribution&&>(contribution));
    }

    bool ParallelBakeEnabled(const editor::EditorContext& context)
    {
        foundation::settings::Settings* store = context.UserEditorSettings();
        if (store == nullptr)
        {
            return true; // headless or in tests: the default
        }
        const NavigationEditorSettings* section = store->Find<NavigationEditorSettings>();
        return section == nullptr || section->parallelBake;
    }

    usize CollectNavigationGeometry(scene::Scene& scene, scene::EntityHandle zoneEntity,
                                    Float3 zoneExtents, f32 cellSize, Array<Float3>& outVertices,
                                    Array<u32>& outIndices)
    {
        outVertices.Clear();
        outIndices.Clear();
        // Rigid (no scale): the navmesh is baked in world units, so a zone entity's scale must
        // not warp the geometry Recast sees. A zone sharing a scaled entity with its ground would
        // otherwise un-scale that ground to unit size and erode the navmesh to nothing. The
        // runtime places the navmesh with the matching rigid frame (RuntimeZone.world).
        const Float4x4 zoneInv = Inverse(RigidPart(scene.GetWorldMatrix(zoneEntity)));
        const AABB zoneBox =
            AABB::FromCenterExtents(scene.GetWorldPosition(zoneEntity), zoneExtents);

        // The level geometry is what the scene's systems say is static and solid (physics: its
        // static, non-trigger bodies; terrain: its surface), never every mesh: an agent, a car or
        // a dropped prop moves, and baking it would leave a hole where it stood. Sampled surfaces
        // come no finer than the cell size, since Recast re-voxelizes to its own cells anyway.
        Array<Float3> world;
        scene.ForEachSystem(
            [&](scene::SceneSystem& system)
            {
                if (scene::IStaticGeometrySource* source = system.AsStaticGeometrySource())
                {
                    source->CollectStaticGeometry(scene, zoneBox, cellSize, world);
                }
            });
        outVertices.Reserve(world.Size());
        outIndices.Reserve(world.Size());
        for (const Float3& p : world)
        {
            outIndices.PushBack(static_cast<u32>(outVertices.Size()));
            outVertices.PushBack(TransformPoint(p, zoneInv)); // to zone-local
        }
        return outIndices.Size() / 3u;
    }

    String DescribeBake(const BakeResult& result)
    {
        if (result.baked)
        {
            // The bake writes the zone's own asset and leaves the scene untouched: nothing to save.
            return String(u8"Navigation baked into the zone's asset; the scene is unchanged. Cook to "
                          u8"apply.");
        }
        if (result.triangleCount == 0)
        {
            // Nothing was collected: no static geometry touched the zone box (centered on the
            // zone's entity, sized by Extents). Render meshes are not read, so a floor that is
            // only a mesh, with no static body, gives nothing.
            return String(u8"No static geometry inside the zone box. Check the zone's Extents cover "
                          u8"your floor, that the floor has a static, non-trigger Rigid Body (or is a "
                          u8"terrain), and that the zone is placed over it. Render meshes are not "
                          u8"read: what agents walk on and avoid is what the physics collides with.");
        }
        // Geometry was collected but Recast produced no walkable surface: the agent and cell
        // parameters did not fit the geometry.
        return Format(u8"Collected {} triangle(s) but Recast produced no walkable surface. Try a "
                      u8"larger Cell Size or a smaller Agent Radius/Height, and check the surface "
                      u8"is within Agent Max Slope.",
                      result.triangleCount);
    }

    BakeResult BakeNavigationZone(scene::Scene& scene, scene::EntityHandle zoneEntity,
                                  foundation::content::Instance& targetAsset, bool parallelBake)
    {
        BakeResult result;
        auto* zones = scene.GetSystem<engine::navigation::NavMeshZoneComponentManager>();
        if (zones == nullptr)
        {
            return result;
        }
        engine::navigation::NavMeshZoneComponent* zone = zones->Get(zoneEntity);
        if (zone == nullptr)
        {
            return result;
        }

        Array<Float3> verts;
        Array<u32> indices;
        result.triangleCount = CollectNavigationGeometry(scene, zoneEntity, zone->extents,
                                                         zone->cellSize, verts, indices);

        pipeline::NavigationZoneAsset asset;
        if (result.triangleCount > 0)
        {
            const nav::NavigationBakeParams params = ParamsFor(*zone, parallelBake);

            Array<byte> blob;
            // Tiled: small zones come out as one tile, large ones split, and the per-tile
            // primitive can regenerate a single tile later. v1 single-tile blobs still load (the
            // reader sniffs the version). Stages are captured on every editor bake and parked on
            // the scene system, so the debugDrawBakeStages overlay shows how this bake arrived
            // at its mesh.
            nav::NavigationBakeStages stages;
            const Status baked = nav::NavigationMeshBuilder::BuildTiled(
                Span<const Float3>{verts.Data(), verts.Size()},
                Span<const u32>{indices.Data(), indices.Size()}, params, blob, &stages);
            if (auto* system = scene.GetSystem<engine::navigation::NavigationSceneSystem>())
            {
                system->SetBakeStages(
                    zoneEntity, static_cast<Array<Float3>&&>(stages.contourLines),
                    static_cast<Array<Float3>&&>(stages.walkableSamples));
            }
            if (baked.IsOk() && !blob.IsEmpty())
            {
                asset.navMeshBlob.Resize(blob.Size());
                MemCopy(asset.navMeshBlob.Data(), blob.Data(), blob.Size());
                result.baked = true;
            }
        }

        if (!pipeline::WriteNavigationZoneAsset(targetAsset, asset).IsOk())
        {
            LOG_ERROR(u8"Editor", u8"navigation bake: writing the zone asset failed");
            result.baked = false;
        }
        return result;
    }


    RegionRebakeResult RebakeNavigationZoneRegion(scene::Scene& scene,
                                                  scene::EntityHandle zoneEntity,
                                                  foundation::content::Instance& targetAsset,
                                                  Float3 worldMin, Float3 worldMax,
                                                  bool parallelBake)
    {
        RegionRebakeResult result;
        auto* zones = scene.GetSystem<engine::navigation::NavMeshZoneComponentManager>();
        engine::navigation::NavMeshZoneComponent* zone =
            (zones != nullptr) ? zones->Get(zoneEntity) : nullptr;
        if (zone == nullptr)
        {
            return result;
        }

        // The existing blob's grid anchors the patch; with no tiled blob it falls back to a full
        // bake (which is also the migration path for v1 assets).
        pipeline::NavigationZoneAsset asset;
        nav::NavigationTileGridDesc grid;
        const bool haveTiled =
            pipeline::EnsureNavMeshLoaded(targetAsset, asset).IsOk() &&
            !asset.navMeshBlob.IsEmpty() &&
            nav::ReadTiledBlobGrid(
                Span<const byte>{reinterpret_cast<const byte*>(asset.navMeshBlob.Data()),
                                 asset.navMeshBlob.Size()},
                grid);
        if (!haveTiled)
        {
            const BakeResult full =
                BakeNavigationZone(scene, zoneEntity, targetAsset, parallelBake);
            result.rebaked = full.baked;
            result.fullBake = true;
            return result;
        }

        Array<Float3> verts;
        Array<u32> indices;
        if (CollectNavigationGeometry(scene, zoneEntity, zone->extents, zone->cellSize, verts,
                                      indices) == 0)
        {
            return result; // nothing to bake against: leave the asset alone
        }

        const nav::NavigationBakeParams params = ParamsFor(*zone, parallelBake);

        // The edited region in zone-local space (the grid's frame), padded by the bake's
        // border apron so tiles whose border overlapped the edit also refresh.
        const Float4x4 zoneInv = Inverse(RigidPart(scene.GetWorldMatrix(zoneEntity)));
        AABB region = AABB::Empty();
        for (int i = 0; i < 8; ++i)
        {
            const Float3 corner{(i & 1) ? worldMax.x : worldMin.x,
                                (i & 2) ? worldMax.y : worldMin.y,
                                (i & 4) ? worldMax.z : worldMin.z};
            region.Expand(TransformPoint(corner, zoneInv));
        }
        const f32 apron =
            (Ceil(params.agentRadius / params.cellSize) + 3.0f) * params.cellSize;

        const i32 tx0 = Clamp(
            static_cast<i32>(Floor((region.min.x - apron - grid.origin.x) / grid.tileWorldSize)),
            0, grid.countX - 1);
        const i32 tx1 = Clamp(
            static_cast<i32>(Floor((region.max.x + apron - grid.origin.x) / grid.tileWorldSize)),
            0, grid.countX - 1);
        const i32 ty0 = Clamp(
            static_cast<i32>(Floor((region.min.z - apron - grid.origin.z) / grid.tileWorldSize)),
            0, grid.countY - 1);
        const i32 ty1 = Clamp(
            static_cast<i32>(Floor((region.max.z + apron - grid.origin.z) / grid.tileWorldSize)),
            0, grid.countY - 1);

        Array<byte> blob;
        blob.Resize(asset.navMeshBlob.Size());
        MemCopy(blob.Data(), asset.navMeshBlob.Data(), asset.navMeshBlob.Size());

        const Span<const Float3> vspan{verts.Data(), verts.Size()};
        const Span<const u32> ispan{indices.Data(), indices.Size()};
        for (i32 ty = ty0; ty <= ty1; ++ty)
        {
            for (i32 tx = tx0; tx <= tx1; ++tx)
            {
                Array<byte> tileData;
                const Status tileStatus =
                    nav::NavigationMeshBuilder::BuildTileInGrid(vspan, ispan, params, grid, tx,
                                                                ty, tileData);
                if (!tileStatus.IsOk() && tileStatus.Code() != ErrorCode::NotFound)
                {
                    return result; // a hard bake failure leaves the asset untouched
                }
                // NotFound means the tile is now empty, so the patch removes its record.
                if (!nav::PatchTiledNavMeshBlob(
                        blob, tx, ty, Span<const byte>{tileData.Data(), tileData.Size()}))
                {
                    return result;
                }
                ++result.tilesRebuilt;
            }
        }

        asset.navMeshBlob.Resize(blob.Size());
        MemCopy(asset.navMeshBlob.Data(), blob.Data(), blob.Size());
        if (!pipeline::WriteNavigationZoneAsset(targetAsset, asset).IsOk())
        {
            return result;
        }
        result.rebaked = true;
        return result;
    }

}
