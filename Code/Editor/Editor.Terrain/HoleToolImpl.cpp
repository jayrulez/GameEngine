// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Terrain - :hole implementation (TerrainHoleTool, its stroke command and the save path).
module;
#include "Core/Prelude.h"

module editor.terrain;

import foundation.core;
import foundation.scene;
import foundation.render;
import foundation.shell;
import foundation.content;
import foundation.resource;
import foundation.heightfield;
import foundation.heightfield.resource;
import heightfield.pipeline;
import foundation.terrain.resource;
import engine.terrain;
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
        [[nodiscard]] engine::terrain::TerrainComponentManager* HoleTerrainManager(scene::Scene& scene)
        {
            scene::ComponentManagerBase* base =
                scene.FindManagerByComponentType(TypeOf<engine::terrain::TerrainComponent>());
            return static_cast<engine::terrain::TerrainComponentManager*>(base);
        }

        // The region-delta stroke command over the hole plane. It replays the touched rect
        // between its before and after byte blocks, is pushed once per stroke, never merges, and
        // keeps the grid alive.
        class HoleStrokeCommand final : public IEditorCommand
        {
        public:
            HoleStrokeCommand(RefPtr<hf::Heightfield> grid, hf::HeightfieldRegion region,
                              Array<u8> before, Array<u8> after)
                : m_grid(Move(grid)), m_region(region), m_before(Move(before)), m_after(Move(after))
            {
            }
            [[nodiscard]] bool Execute() override
            {
                Write(m_after); // a no-op on push (the stroke already cut it); the replay on redo
                return !m_region.IsEmpty();
            }
            void Undo() override { Write(m_before); }
            [[nodiscard]] StringView TypeId() const override { return u8"terrain.hole.stroke"; }

        private:
            void Write(const Array<u8>& block)
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
                        m_grid->SetHole(m_region.minX + x, m_region.minZ + z,
                                        block[static_cast<usize>(z) * static_cast<usize>(w) +
                                              static_cast<usize>(x)] != 0);
                    }
                }
                m_grid->BumpVersion(); // the chunk meshes, the collider and the grass update from it
            }
            RefPtr<hf::Heightfield> m_grid;
            hf::HeightfieldRegion m_region;
            Array<u8> m_before;
            Array<u8> m_after;
        };

        [[nodiscard]] Array<u8> SliceHoleRegion(Span<const u8> full, i32 gridSize,
                                                const hf::HeightfieldRegion& r)
        {
            Array<u8> out;
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

    bool TerrainHoleTool::IsAvailable() const
    {
        engine::terrain::TerrainComponentManager* mgr = HoleTerrainManager(*m_scene);
        if (mgr == nullptr)
        {
            return false;
        }
        bool any = false;
        mgr->ForEach(
            [&](engine::terrain::TerrainComponent& c, scene::EntityHandle)
            {
                foundation::terrain::TerrainResource* res = c.terrain.Get();
                hf::Heightfield* grid = (res != nullptr) ? res->heightfield.Get() : nullptr;
                if (grid != nullptr && !grid->IsEmpty())
                {
                    any = true;
                }
            });
        return any;
    }

    TerrainHoleTool::Pick TerrainHoleTool::ResolvePick(const ViewportToolInput& input) const
    {
        Pick best;
        engine::terrain::TerrainComponentManager* mgr = HoleTerrainManager(*m_scene);
        if (mgr == nullptr)
        {
            return best;
        }
        f32 bestDist = kFloatMax;
        mgr->ForEach(
            [&](engine::terrain::TerrainComponent& c, scene::EntityHandle owner)
            {
                foundation::terrain::TerrainResource* res = c.terrain.Get();
                hf::Heightfield* grid = (res != nullptr) ? res->heightfield.Get() : nullptr;
                if (grid == nullptr || grid->IsEmpty())
                {
                    return;
                }
                const Float4x4 world = m_scene->GetWorldMatrix(owner);
                const Float4x4 inv = Inverse(world);
                const Float3 localOrigin = TransformPoint(input.ray.origin, inv);
                const Float3 localDir = TransformDirection(input.ray.direction, inv);
                f32 t = 0.0f;
                // The brush works on the hole plane. QueryRay would pass through a cut, so Fill
                // could only land from the rim; this pick treats cut samples as surface.
                if (!grid->QueryRayIgnoringHoles(localOrigin, localDir, t))
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
                best.worldHit = worldHit;
                best.worldNormal =
                    Normalized(TransformDirection(grid->GetNormalAt(localHit.x, localHit.z), world));
                best.valid = true;
            });
        return best;
    }

    bool TerrainHoleTool::Update(const ViewportToolInput& input)
    {
        m_hasHover = false;
        if (foundation::shell::IKeyboard* kb = input.keyboard; kb != nullptr && input.pointerValid)
        {
            using foundation::shell::KeyCode;
            if (kb->IsKeyPressed(KeyCode::Num1)) SetMode(Mode::Cut);
            if (kb->IsKeyPressed(KeyCode::Num2)) SetMode(Mode::Fill);
        }
        // Shift and the wheel resize the brush; the wheel alone stays the camera's dolly.
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
        if (!input.editingLocked) // no edits while simulating, since the collider is shared
        {
            if (!m_stroking && pick.valid && input.pointerOver && input.leftPressed)
            {
                BeginStroke(pick);
                consumed = true;
            }
            else if (m_stroking && input.leftDown && pick.valid && pick.hf == m_strokeHf.Get())
            {
                ApplyDab(pick);
                consumed = true;
            }
            if (m_stroking && (input.leftReleased || !input.pointerValid))
            {
                EndStroke();
            }
        }
        else if (m_stroking)
        {
            EndStroke();
        }
        UpdateStatus();
        return consumed;
    }

    void TerrainHoleTool::BeginStroke(const Pick& pick)
    {
        m_stroking = true;
        m_strokeHf = RefPtr<hf::Heightfield>(pick.hf);
        m_strokeHeightfieldId = pick.heightfieldId;
        const Span<const u8> holes = pick.hf->Holes();
        m_before.Resize(holes.Size());
        if (!holes.IsEmpty())
        {
            MemCopy(m_before.Data(), holes.Data(), holes.Size());
        }
        m_region = hf::HeightfieldRegion{};
        ApplyDab(pick);
    }

    void TerrainHoleTool::ApplyDab(const Pick& pick)
    {
        if (m_strokeHf.Get() == nullptr)
        {
            return;
        }
        const hf::HeightfieldRegion r =
            (m_mode == Mode::Cut) ? hf::CutHoles(*m_strokeHf, pick.localX, pick.localZ, m_radius)
                                  : hf::FillHoles(*m_strokeHf, pick.localX, pick.localZ, m_radius);
        if (!r.IsEmpty())
        {
            m_region.Add(r.minX, r.minZ);
            m_region.Add(r.maxX, r.maxZ);
        }
    }

    void TerrainHoleTool::EndStroke()
    {
        const bool hadRegion = m_stroking && m_strokeHf.Get() != nullptr && !m_region.IsEmpty();
        if (hadRegion)
        {
            const i32 gridSize = m_strokeHf->Size();
            Array<u8> before = SliceHoleRegion(Span<const u8>{m_before.Data(), m_before.Size()},
                                               gridSize, m_region);
            Array<u8> after = SliceHoleRegion(m_strokeHf->Holes(), gridSize, m_region);
            bool changed = false;
            for (usize i = 0; i < before.Size() && !changed; ++i)
            {
                changed = before[i] != after[i];
            }
            if (changed) // a fill over solid ground (or a cut over a cut) pushes no command
            {
                m_commands->Execute(UniquePtr<IEditorCommand>(
                    editor::EditorRootAllocator().New<HoleStrokeCommand>(m_strokeHf, m_region,
                                                                         Move(before), Move(after)),
                    editor::EditorRootAllocator()));
                if (m_assetEdits != nullptr && !m_strokeHeightfieldId.IsNil())
                {
                    RefPtr<hf::Heightfield> grid = m_strokeHf;
                    const Guid id = m_strokeHeightfieldId;
                    m_assetEdits->RegisterAssetEdit(
                        id,
                        [grid, id](content::ContentDatabase& db) -> Status
                        {
                            // Write the source HeightfieldAsset, never the cooked type: sync the
                            // params, clear fileName (the sidecars become authoritative), then
                            // write both streams. The heights are written too, or an imported
                            // heightfield would lose the heights from its image as soon as its
                            // file name is cleared.
                            content::Instance* inst = db.GetInstance(id);
                            if (inst == nullptr || grid.Get() == nullptr)
                            {
                                return Status{ErrorCode::NotFound};
                            }
                            RefPtr<ISerializable> object = inst->ReadObject();
                            auto* asset = Cast<pipeline::HeightfieldAsset>(object.Get());
                            if (asset == nullptr)
                            {
                                return Status{ErrorCode::InvalidArgument};
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
                            const Status heights = inst->WriteData(
                                hf::kHeightStream, hf::HeightfieldSource::HeightBlob(*grid));
                            if (!heights.IsOk())
                            {
                                return heights;
                            }
                            return inst->WriteData(hf::kHoleStream,
                                                   hf::HeightfieldSource::HoleBlob(*grid));
                        });
                }
            }
        }
        m_stroking = false;
        m_strokeHf = nullptr;
        m_before.Clear();
        m_region = hf::HeightfieldRegion{};
    }

    void TerrainHoleTool::OnDeactivate()
    {
        if (m_stroking)
        {
            EndStroke();
        }
        m_hasHover = false;
    }

    void TerrainHoleTool::Draw(render::debug::DebugDraw& drawList)
    {
        if (!m_hasHover)
        {
            return;
        }
        // A hard-edged disc: one ring, magenta to cut, green to fill.
        const Color ring = (m_mode == Mode::Cut) ? Color{1.0f, 0.25f, 0.9f, 1.0f}
                                                 : Color{0.35f, 1.0f, 0.45f, 1.0f};
        drawList.DrawCircleNormal(m_hoverWorld, m_radius, m_hoverNormal, ring, 40, true);
    }

    void TerrainHoleTool::UpdateStatus()
    {
        m_status = Format(u8"Cut Holes [{}]  radius {}  (1 cut, 2 fill, Shift+wheel size)",
                          m_mode == Mode::Cut ? u8"CUT" : u8"FILL",
                          static_cast<i32>(m_radius + 0.5f));
    }
}
