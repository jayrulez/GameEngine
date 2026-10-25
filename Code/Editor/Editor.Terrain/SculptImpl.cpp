// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - :sculpt implementation (TerrainSculptTool + provider + registrar).
//
// The heavy engine/resource imports the interface partition deliberately keeps out (GCC module
// hygiene) live here: the terrain component + manager (to find the terrain in the scene and its
// shared heightfield) and the heightfield source (to serialize the runtime grid back on save).

module;
#include "Core/Prelude.h"

module editor.terrain;

import foundation.core;
import foundation.scene;
import foundation.render;              // debug::DebugDraw
import foundation.shell;              // IKeyboard (mode hotkeys)
import foundation.content;            // ContentDatabase, Instance (the persist closure)
import foundation.resource;           // Ref<>
import foundation.heightfield;        // Sculpt* brushes, HeightfieldRegion
import foundation.heightfield.resource; // HeightfieldSource + kHeightStream (persist)
import heightfield.pipeline;           // HeightfieldAsset (the SOURCE envelope the persist rewrites)
import foundation.terrain.resource;   // TerrainResource
import engine.terrain;                // TerrainComponent + TerrainComponentManager
import editor.core;
import editor.viewporttools;

using namespace foundation::core;

namespace editor
{
    namespace scene = foundation::scene;
    namespace hf = foundation::heightfield;
    namespace render = foundation::render;
    namespace content = foundation::content;

    namespace
    {
        // The scene's terrain component manager (serialization id "terrain"), or null.
        [[nodiscard]] engine::terrain::TerrainComponentManager* TerrainManager(scene::Scene& scene)
        {
            scene::ComponentManagerBase* base =
                scene.FindManagerByComponentType(TypeOf<engine::terrain::TerrainComponent>());
            return static_cast<engine::terrain::TerrainComponentManager*>(base);
        }

        // The region-delta stroke command: replays the touched grid RECTANGLE between its before /
        // after sample blocks. One per stroke (press..release); never merges. Keeps the grid alive.
        class SculptStrokeCommand final : public IEditorCommand
        {
        public:
            SculptStrokeCommand(RefPtr<hf::Heightfield> grid, hf::HeightfieldRegion region,
                                Array<hf::Height> before, Array<hf::Height> after)
                : m_grid(Move(grid)), m_region(region), m_before(Move(before)), m_after(Move(after))
            {
            }

            [[nodiscard]] bool Execute() override
            {
                // The live grid is already in the AFTER state when the command is first pushed (the
                // stroke painted it), so writing AFTER here is a no-op on push and the correct
                // replay on redo. Bump either way so the GPU height texture re-uploads.
                Write(m_after);
                return !m_region.IsEmpty();
            }
            void Undo() override { Write(m_before); }
            [[nodiscard]] StringView TypeId() const override { return u8"terrain.sculpt.stroke"; }

        private:
            void Write(const Array<hf::Height>& block)
            {
                if (m_grid.Get() == nullptr || m_region.IsEmpty())
                {
                    return;
                }
                const i32 w = m_region.Width();
                for (i32 z = 0; z < m_region.Height(); ++z)
                {
                    for (i32 x = 0; x < w; ++x)
                    {
                        m_grid->SetSample(m_region.minX + x, m_region.minZ + z,
                                          block[static_cast<usize>(z) * static_cast<usize>(w) +
                                                static_cast<usize>(x)]);
                    }
                }
                m_grid->BumpVersion();
            }

            RefPtr<hf::Heightfield> m_grid;
            hf::HeightfieldRegion m_region;
            Array<hf::Height> m_before;
            Array<hf::Height> m_after;
        };

        // Slice the inclusive region out of a full-grid sample array (row-major, side = grid size).
        [[nodiscard]] Array<hf::Height> SliceRegion(Span<const hf::Height> full, i32 gridSize,
                                                    const hf::HeightfieldRegion& r)
        {
            Array<hf::Height> out;
            if (r.IsEmpty())
            {
                return out;
            }
            const i32 w = r.Width();
            const i32 h = r.Height();
            out.Resize(static_cast<usize>(w) * static_cast<usize>(h));
            for (i32 z = 0; z < h; ++z)
            {
                for (i32 x = 0; x < w; ++x)
                {
                    const usize src = static_cast<usize>(r.minZ + z) * static_cast<usize>(gridSize) +
                                      static_cast<usize>(r.minX + x);
                    out[static_cast<usize>(z) * static_cast<usize>(w) + static_cast<usize>(x)] =
                        full[src];
                }
            }
            return out;
        }
    }

    bool TerrainSculptTool::IsAvailable() const
    {
        engine::terrain::TerrainComponentManager* mgr = TerrainManager(*m_scene);
        if (mgr == nullptr)
        {
            return false;
        }
        bool any = false;
        mgr->ForEach(
            [&](engine::terrain::TerrainComponent& c, scene::EntityHandle)
            {
                foundation::terrain::TerrainResource* res = c.terrain.Get();
                if (res == nullptr)
                {
                    return;
                }
                foundation::heightfield::Heightfield* hf = res->heightfield.Get();
                if (hf == nullptr || hf->IsEmpty())
                {
                    return;
                }
                any = true;
            });
        return any;
    }

    TerrainSculptTool::Pick TerrainSculptTool::ResolvePick(const ViewportToolInput& input) const
    {
        Pick best;
        engine::terrain::TerrainComponentManager* mgr = TerrainManager(*m_scene);
        if (mgr == nullptr)
        {
            return best;
        }
        f32 bestDist = kFloatMax;
        mgr->ForEach(
            [&](engine::terrain::TerrainComponent& c, scene::EntityHandle owner)
            {
                foundation::terrain::TerrainResource* res = c.terrain.Get();
                if (res == nullptr)
                {
                    return;
                }
                hf::Heightfield* grid = res->heightfield.Get();
                if (grid == nullptr || grid->IsEmpty())
                {
                    return;
                }
                const Float4x4 world = m_scene->GetWorldMatrix(owner);
                const Float4x4 inv = Inverse(world);
                const Float3 localOrigin = TransformPoint(input.ray.origin, inv);
                const Float3 localDir = TransformDirection(input.ray.direction, inv);
                f32 t = 0.0f;
                if (!grid->QueryRay(localOrigin, localDir, t))
                {
                    return;
                }
                const Float3 localHit = localOrigin + Normalized(localDir) * t;
                const Float3 worldHit = TransformPoint(localHit, world);
                const f32 dist = Length(worldHit - input.ray.origin);
                if (dist >= bestDist)
                {
                    return;
                }
                bestDist = dist;
                best.hf = grid;
                best.heightfieldId = res->heightfield.id;
                best.localX = localHit.x;
                best.localZ = localHit.z;
                best.localY = localHit.y;
                best.worldHit = worldHit;
                const Float3 localN = grid->GetNormalAt(localHit.x, localHit.z);
                best.worldNormal = Normalized(TransformDirection(localN, world));
                best.valid = true;
            });
        return best;
    }

    bool TerrainSculptTool::Update(const ViewportToolInput& input)
    {
        m_hasHover = false;

        // Mode hotkeys (1..4) - a pressed key sets the mode (idempotent; the panel mirrors it).
        if (foundation::shell::IKeyboard* kb = input.keyboard; kb != nullptr && input.pointerValid)
        {
            using foundation::shell::KeyCode;
            if (kb->IsKeyPressed(KeyCode::Num1)) m_mode = Mode::Raise;
            if (kb->IsKeyPressed(KeyCode::Num2)) m_mode = Mode::Lower;
            if (kb->IsKeyPressed(KeyCode::Num3)) m_mode = Mode::Smooth;
            if (kb->IsKeyPressed(KeyCode::Num4)) m_mode = Mode::Flatten;
        }

        // Wheel resizes the brush (multiplicative so it feels even across scales).
        // Shift + wheel resizes the brush; the bare wheel stays the camera's dolly (2026-09-23).
        if (input.pointerOver && input.shift && input.wheelDelta != 0.0f)
        {
            SetRadius(m_radius * (1.0f + 0.12f * input.wheelDelta));
        }

        const Pick pick = ResolvePick(input);
        if (pick.valid)
        {
            m_hasHover = true;
            m_hoverWorld = pick.worldHit;
            m_hoverNormal = pick.worldNormal;
        }

        bool consumed = m_stroking;

        // Ctrl+click over terrain picks the flatten target height (not an edit).
        if (pick.valid && input.pointerOver && input.leftPressed && input.ctrl)
        {
            m_flattenTarget = pick.localY;
            m_hasFlattenTarget = true;
            m_mode = Mode::Flatten;
            UpdateStatus();
            return true;
        }

        if (!input.editingLocked) // Simulate: the collider is shared - no edits
        {
            if (!m_stroking && pick.valid && input.pointerOver && input.leftPressed && !input.ctrl)
            {
                BeginStroke(pick);
                consumed = true;
            }
            else if (m_stroking && input.leftDown && pick.valid && pick.hf == m_strokeHf.Get())
            {
                ApplyDab(pick, input.deltaSeconds);
                consumed = true;
            }

            if (m_stroking && (input.leftReleased || !input.pointerValid))
            {
                EndStroke();
            }
        }
        else if (m_stroking)
        {
            EndStroke(); // Simulate started mid-stroke: commit what was painted, then stop
        }

        UpdateStatus();
        return consumed;
    }

    void TerrainSculptTool::BeginStroke(const Pick& pick)
    {
        m_stroking = true;
        m_strokeHf = RefPtr<hf::Heightfield>(pick.hf); // keep the grid alive for the whole stroke
        m_strokeHeightfieldId = pick.heightfieldId;
        const Span<const hf::Height> samples = pick.hf->Samples();
        m_before.Resize(samples.Size());
        if (!samples.IsEmpty())
        {
            MemCopy(m_before.Data(), samples.Data(), samples.Size() * sizeof(hf::Height));
        }
        m_region = hf::HeightfieldRegion{};
        ApplyDab(pick, 0.0f); // an instant single-click still deposits one dab (dt=0 -> min delta)
    }

    void TerrainSculptTool::ApplyDab(const Pick& pick, f32 deltaSeconds)
    {
        if (m_strokeHf.Get() == nullptr)
        {
            return;
        }
        // A continuous brush: the world-Y delta this frame is strength * dt. A zero-dt dab (the
        // instant single click) still deposits a minimum so a click is never a no-op.
        const f32 step = m_strength * (deltaSeconds > 0.0f ? deltaSeconds : (1.0f / 60.0f));
        hf::HeightfieldRegion r;
        switch (m_mode)
        {
        case Mode::Raise:
            r = hf::SculptRaise(*m_strokeHf, pick.localX, pick.localZ, m_radius, +step);
            break;
        case Mode::Lower:
            r = hf::SculptRaise(*m_strokeHf, pick.localX, pick.localZ, m_radius, -step);
            break;
        case Mode::Smooth:
            // Smooth/flatten "amount" is a 0..1 blend; scale the per-second rate but cap so a slow
            // frame cannot overshoot the target.
            r = hf::SculptSmooth(*m_strokeHf, pick.localX, pick.localZ, m_radius,
                                 Clamp(step * 0.5f, 0.0f, 1.0f));
            break;
        case Mode::Flatten:
        {
            const f32 target = m_hasFlattenTarget ? m_flattenTarget : pick.localY;
            r = hf::SculptFlatten(*m_strokeHf, pick.localX, pick.localZ, m_radius,
                                  Clamp(step * 0.5f, 0.0f, 1.0f), target);
            break;
        }
        }
        // Union the touched rect into the stroke region.
        if (!r.IsEmpty())
        {
            m_region.Add(r.minX, r.minZ);
            m_region.Add(r.maxX, r.maxZ);
        }
    }

    void TerrainSculptTool::EndStroke()
    {
        const bool hadRegion = m_stroking && m_strokeHf.Get() != nullptr && !m_region.IsEmpty();
        if (hadRegion)
        {
            const i32 gridSize = m_strokeHf->Size();
            Array<hf::Height> before =
                SliceRegion(Span<const hf::Height>{m_before.Data(), m_before.Size()}, gridSize,
                            m_region);
            Array<hf::Height> after = SliceRegion(m_strokeHf->Samples(), gridSize, m_region);
            m_commands->Execute(UniquePtr<IEditorCommand>(
                editor::EditorRootAllocator().New<SculptStrokeCommand>(m_strokeHf, m_region, Move(before),
                                                            Move(after)),
                editor::EditorRootAllocator()));

            // Register the write-back-to-source persist closure (drained on Save). Captures a
            // RefPtr to the grid (kept alive) + its source guid; the DB is handed in at drain time.
            if (m_assetEdits != nullptr && !m_strokeHeightfieldId.IsNil())
            {
                RefPtr<hf::Heightfield> grid = m_strokeHf;
                const Guid id = m_strokeHeightfieldId;
                m_assetEdits->RegisterAssetEdit(
                    id,
                    [grid, id](content::ContentDatabase& db) -> Status
                    {
                        // The SOURCE instance's envelope is a HeightfieldAsset (never write the
                        // cooked HeightfieldSource type here - that clobbers the source asset).
                        // Read-modify-write: sync the grid params, CLEAR fileName (the authored
                        // "heights" sidecar becomes the truth - the editable-source convention;
                        // the builder's embedded path cooks from it, and a re-import explicitly
                        // resets by setting fileName again), then write the sidecar samples.
                        content::Instance* inst = db.GetInstance(id);
                        if (inst == nullptr || grid.Get() == nullptr)
                        {
                            return Status{ErrorCode::NotFound}; // heightfield source vanished
                        }
                        RefPtr<ISerializable> object = inst->ReadObject();
                        auto* asset = Cast<pipeline::HeightfieldAsset>(object.Get());
                        if (asset == nullptr)
                        {
                            return Status{ErrorCode::InvalidArgument}; // not a heightfield asset
                        }
                        asset->fileName = {};
                        asset->size = grid->Size();
                        asset->worldSize = grid->WorldSize();
                        asset->minY = grid->MinY();
                        asset->maxY = grid->MaxY();
                        const Status wrote = inst->WriteObject(*asset);
                        if (!wrote.IsOk())
                        {
                            return wrote;
                        }
                        return inst->WriteData(hf::kHeightStream,
                                               hf::HeightfieldSource::HeightBlob(*grid));
                    });
            }
        }

        m_stroking = false;
        m_strokeHf = nullptr;
        m_before.Clear();
        m_region = hf::HeightfieldRegion{};
    }

    void TerrainSculptTool::OnDeactivate()
    {
        if (m_stroking)
        {
            EndStroke(); // gesture-end guarantee: never leave a half-open stroke on a tool switch
        }
        m_hasHover = false;
    }

    void TerrainSculptTool::Draw(render::debug::DebugDraw& drawList)
    {
        if (!m_hasHover)
        {
            return;
        }
        // A two-ring cursor on the surface: outer = radius, inner = the falloff half-power point.
        const Color ring = (m_mode == Mode::Lower) ? Color{1.0f, 0.55f, 0.2f, 1.0f}
                                                   : Color{0.3f, 0.9f, 1.0f, 1.0f};
        drawList.DrawCircleNormal(m_hoverWorld, m_radius, m_hoverNormal, ring, 40, true);
        drawList.DrawCircleNormal(m_hoverWorld, m_radius * 0.5f, m_hoverNormal,
                                  Color{ring.r, ring.g, ring.b, 0.5f}, 32, true);
    }

    void TerrainSculptTool::UpdateStatus()
    {
        StringView modeText = u8"Raise";
        switch (m_mode)
        {
        case Mode::Raise: modeText = u8"Raise"; break;
        case Mode::Lower: modeText = u8"Lower"; break;
        case Mode::Smooth: modeText = u8"Smooth"; break;
        case Mode::Flatten: modeText = u8"Flatten"; break;
        }
        m_status = Format(u8"Sculpt [{}]  radius {}  strength {}  (1-4 mode, Shift+wheel size, Ctrl+click "
                          u8"= flatten target)",
                          modeText, static_cast<i32>(m_radius + 0.5f),
                          static_cast<i32>(m_strength + 0.5f));
    }

    void TerrainViewportToolProvider::CreateTools(ViewportToolManager& manager,
                                                  const ViewportToolHostContext& context)
    {
        if (context.scene == nullptr || context.commands == nullptr)
        {
            return;
        }
        manager.Add(UniquePtr<IViewportTool>(
            editor::EditorRootAllocator().New<TerrainSculptTool>(*context.scene, *context.commands,
                                                      context.assetEdits),
            editor::EditorRootAllocator()));
        manager.Add(UniquePtr<IViewportTool>(
            editor::EditorRootAllocator().New<TerrainSplatTool>(*context.scene, *context.commands,
                                                     context.assetEdits),
            editor::EditorRootAllocator()));
        manager.Add(UniquePtr<IViewportTool>(
            editor::EditorRootAllocator().New<TerrainHoleTool>(*context.scene, *context.commands,
                                                               context.assetEdits),
            editor::EditorRootAllocator()));
    }

    void RegisterTerrainViewportTools()
    {
        static TerrainViewportToolProvider provider;
        ViewportToolProviderRegistry::Get().Register(&provider);
    }
}
