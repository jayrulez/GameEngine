// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - :splat implementation (TerrainSplatTool). Also extends the terrain viewport-tool
// provider (SculptImpl.cpp) to contribute this second tool.
//
// The heavy engine/resource imports the interface partition keeps out (GCC hygiene) live here: the
// terrain component + manager (to find the terrain + its shared splatmap) and the splatmap source
// (to serialize the painted RGBA8 back on save).

module;
#include "Core/Prelude.h"

module editor.terrain;

import foundation.core;
import foundation.scene;
import foundation.render;            // debug::DebugDraw
import foundation.shell;            // IKeyboard (layer hotkeys)
import foundation.content;          // ContentDatabase, Instance (the persist closure)
import foundation.resource;         // Ref<>
import foundation.heightfield;      // Heightfield (ray-pick for the UV mapping)
import foundation.terrain.resource; // SplatWeights, PaintTopK/EraseTopK, SplatWeightsSource
import terrain.pipeline;            // SplatmapAsset (the SOURCE envelope the persist rewrites)
import engine.terrain;              // TerrainComponent + TerrainComponentManager
import editor.core;
import editor.viewporttools;

using namespace foundation::core;

namespace editor
{
    namespace scene = foundation::scene;
    namespace terrain = foundation::terrain;
    namespace hf = foundation::heightfield;
    namespace render = foundation::render;
    namespace content = foundation::content;

    namespace
    {
        [[nodiscard]] engine::terrain::TerrainComponentManager* SplatTerrainManager(scene::Scene& scene)
        {
            scene::ComponentManagerBase* base =
                scene.FindManagerByComponentType(TypeOf<engine::terrain::TerrainComponent>());
            return static_cast<engine::terrain::TerrainComponentManager*>(base);
        }

        // The region-delta stroke command over BOTH top-K rasters (indices + weights): replays
        // the touched pixel RECTANGLE between its before / after blocks. One per stroke; never
        // merges. Keeps the rasters alive.
        class SplatStrokeCommand final : public IEditorCommand
        {
        public:
            SplatStrokeCommand(RefPtr<terrain::SplatWeights> weights, terrain::SplatRegion region,
                               Array<u8> beforeW, Array<u8> afterW, Array<u8> beforeI,
                               Array<u8> afterI)
                : m_weights(Move(weights)), m_region(region), m_beforeW(Move(beforeW)),
                  m_afterW(Move(afterW)), m_beforeI(Move(beforeI)), m_afterI(Move(afterI))
            {
            }

            [[nodiscard]] bool Execute() override
            {
                Write(m_afterW, m_afterI); // live rasters are already AFTER on push; replays on redo
                return !m_region.IsEmpty();
            }
            void Undo() override { Write(m_beforeW, m_beforeI); }
            [[nodiscard]] StringView TypeId() const override { return u8"terrain.splat.stroke"; }

        private:
            void Write(const Array<u8>& weightBlock, const Array<u8>& indexBlock)
            {
                if (m_weights.Get() == nullptr || m_region.IsEmpty())
                {
                    return;
                }
                const i32 w = m_region.Width();
                Span<u8> wp = m_weights->Weights();
                Span<u8> ip = m_weights->Indices();
                const i32 rasterW = m_weights->Width();
                for (i32 y = 0; y < m_region.Height(); ++y)
                {
                    for (i32 x = 0; x < w; ++x)
                    {
                        const usize dst = (static_cast<usize>(m_region.minY + y) *
                                               static_cast<usize>(rasterW) +
                                           static_cast<usize>(m_region.minX + x)) *
                                          4u;
                        const usize src =
                            (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) *
                            4u;
                        for (u32 k = 0; k < 4; ++k)
                        {
                            wp[dst + k] = weightBlock[src + k];
                            ip[dst + k] = indexBlock[src + k];
                        }
                    }
                }
                m_weights->BumpVersion();
            }

            RefPtr<terrain::SplatWeights> m_weights;
            terrain::SplatRegion m_region;
            Array<u8> m_beforeW;
            Array<u8> m_afterW;
            Array<u8> m_beforeI;
            Array<u8> m_afterI;
        };

        // Slice the inclusive pixel region (RGBA8) out of a full-raster pixel array.
        [[nodiscard]] Array<u8> SliceRegion(Span<const u8> full, i32 rasterW,
                                            const terrain::SplatRegion& r)
        {
            Array<u8> out;
            if (r.IsEmpty())
            {
                return out;
            }
            const i32 w = r.Width();
            const i32 h = r.Height();
            out.Resize(static_cast<usize>(w) * static_cast<usize>(h) * 4u);
            for (i32 y = 0; y < h; ++y)
            {
                for (i32 x = 0; x < w; ++x)
                {
                    const usize src = (static_cast<usize>(r.minY + y) * static_cast<usize>(rasterW) +
                                       static_cast<usize>(r.minX + x)) *
                                      4u;
                    const usize dst =
                        (static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)) * 4u;
                    for (u32 k = 0; k < 4; ++k)
                    {
                        out[dst + k] = full[src + k];
                    }
                }
            }
            return out;
        }
    }

    bool TerrainSplatTool::IsAvailable() const
    {
        engine::terrain::TerrainComponentManager* mgr = SplatTerrainManager(*m_scene);
        if (mgr == nullptr)
        {
            return false;
        }
        bool any = false;
        mgr->ForEach(
            [&](engine::terrain::TerrainComponent& c, scene::EntityHandle)
            {
                if (any)
                {
                    return;
                }
                terrain::TerrainResource* res = c.terrain.Get();
                if (res != nullptr && res->weights.Get() != nullptr &&
                    !res->weights.Get()->IsEmpty() && res->heightfield.Get() != nullptr &&
                    !res->heightfield.Get()->IsEmpty())
                {
                    any = true;
                }
            });
        return any;
    }

    TerrainSplatTool::Pick TerrainSplatTool::ResolvePick(const ViewportToolInput& input) const
    {
        Pick best;
        engine::terrain::TerrainComponentManager* mgr = SplatTerrainManager(*m_scene);
        if (mgr == nullptr)
        {
            return best;
        }
        f32 bestDist = kFloatMax;
        mgr->ForEach(
            [&](engine::terrain::TerrainComponent& c, scene::EntityHandle owner)
            {
                terrain::TerrainResource* res = c.terrain.Get();
                if (res == nullptr)
                {
                    return;
                }
                terrain::SplatWeights* weights = res->weights.Get();
                hf::Heightfield* grid = res->heightfield.Get();
                if (weights == nullptr || weights->IsEmpty() || grid == nullptr || grid->IsEmpty())
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
                // Map the local XZ hit to the 0..1 footprint UV the splatmap covers (the same
                // mapping the shader's splatUV uses: centred on the local origin).
                const Float2 ws = grid->WorldSize();
                const f32 uvX = localHit.x / (ws.x != 0.0f ? ws.x : 1.0f) + 0.5f;
                const f32 uvY = localHit.z / (ws.y != 0.0f ? ws.y : 1.0f) + 0.5f;
                bestDist = dist;
                best.weights = weights;
                best.weightsId = res->weights.id;
                best.uvX = uvX;
                best.uvY = uvY;
                best.worldSizeX = ws.x != 0.0f ? ws.x : 1.0f;
                best.worldSizeY = ws.y != 0.0f ? ws.y : 1.0f;
                best.worldHit = worldHit;
                best.worldNormal = Normalized(TransformDirection(grid->GetNormalAt(localHit.x, localHit.z), world));
                best.valid = true;
            });
        return best;
    }

    bool TerrainSplatTool::Update(const ViewportToolInput& input)
    {
        m_hasHover = false;

        // Palette hotkeys 1..9 -> palette layers 0..8; 0 = the ERASER (reveals the base);
        // minus = SMOOTH (blur toward the neighborhood average). The panel mirrors the
        // selection and offers the full (unbounded) palette.
        if (foundation::shell::IKeyboard* kb = input.keyboard; kb != nullptr && input.pointerValid)
        {
            using foundation::shell::KeyCode;
            if (kb->IsKeyPressed(KeyCode::Num1)) SetPaletteIndex(0);
            if (kb->IsKeyPressed(KeyCode::Num2)) SetPaletteIndex(1);
            if (kb->IsKeyPressed(KeyCode::Num3)) SetPaletteIndex(2);
            if (kb->IsKeyPressed(KeyCode::Num4)) SetPaletteIndex(3);
            if (kb->IsKeyPressed(KeyCode::Num5)) SetPaletteIndex(4);
            if (kb->IsKeyPressed(KeyCode::Num6)) SetPaletteIndex(5);
            if (kb->IsKeyPressed(KeyCode::Num7)) SetPaletteIndex(6);
            if (kb->IsKeyPressed(KeyCode::Num8)) SetPaletteIndex(7);
            if (kb->IsKeyPressed(KeyCode::Num9)) SetPaletteIndex(8);
            if (kb->IsKeyPressed(KeyCode::Num0)) SetEraser(true);
            if (kb->IsKeyPressed(KeyCode::Minus)) SetSmooth(true);
        }

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

        if (!input.editingLocked) // Simulate: the splatmap feeds the live material - no edits
        {
            if (!m_stroking && pick.valid && input.pointerOver && input.leftPressed)
            {
                BeginStroke(pick);
                consumed = true;
            }
            else if (m_stroking && input.leftDown && pick.valid && pick.weights == m_strokeWeights.Get())
            {
                AdvanceStroke(pick, input.deltaSeconds);
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

    void TerrainSplatTool::BeginStroke(const Pick& pick)
    {
        m_stroking = true;
        m_strokeWeights =
            RefPtr<terrain::SplatWeights>(pick.weights); // keep the rasters alive for the stroke
        m_strokeWeightsId = pick.weightsId;
        const Span<const u8> wp = pick.weights->Weights();
        const Span<const u8> ip = pick.weights->Indices();
        m_beforeWeights.Resize(wp.Size());
        m_beforeIndices.Resize(ip.Size());
        if (!wp.IsEmpty())
        {
            MemCopy(m_beforeWeights.Data(), wp.Data(), wp.Size());
            MemCopy(m_beforeIndices.Data(), ip.Data(), ip.Size());
        }
        m_region = terrain::SplatRegion{};
        m_airbrushClock = 0.0f;
        // The press deposits ONE full stamp and anchors the spacing walker there.
        ApplyStamp(pick.uvX, pick.uvY, pick, m_strength);
        m_lastStampU = pick.uvX;
        m_lastStampV = pick.uvY;
    }

    // Distance-spaced stamping (the standard paint-brush model): a stamp lands every m_spacing of
    // the brush radius of world-space pointer travel, WALKING the segment from the last stamp so a
    // fast drag leaves no gaps. Frame-rate and drag-speed independent; holding still deposits
    // nothing beyond the press stamp (scrub to build up at sub-1 strengths) - unless AIRBRUSH is
    // on, which ALSO stamps at the cursor on a fixed time cadence (build-up by hovering).
    constexpr i32 kMaxStampsPerFrame = 1024;  // teleport guard (alt-tab warps, huge drags)
    constexpr f32 kAirbrushPeriod = 0.05f;    // seconds per time-cadence stamp (20/s)

    void TerrainSplatTool::AdvanceStroke(const Pick& pick, f32 deltaSeconds)
    {
        if (m_strokeWeights.Get() == nullptr)
        {
            return;
        }
        const f32 spacing = Max(m_radius * m_spacing, 1e-4f);
        for (i32 i = 0; i < kMaxStampsPerFrame; ++i)
        {
            const f32 dxWorld = (pick.uvX - m_lastStampU) * pick.worldSizeX;
            const f32 dyWorld = (pick.uvY - m_lastStampV) * pick.worldSizeY;
            const f32 dist = Sqrt(dxWorld * dxWorld + dyWorld * dyWorld);
            if (dist < spacing)
            {
                break; // remainder carries to the next frame (no partial stamps)
            }
            const f32 f = spacing / dist;
            m_lastStampU += (pick.uvX - m_lastStampU) * f;
            m_lastStampV += (pick.uvY - m_lastStampV) * f;
            ApplyStamp(m_lastStampU, m_lastStampV, pick, m_strength);
        }
        if (m_airbrush)
        {
            m_airbrushClock += Max(deltaSeconds, 0.0f);
            for (i32 i = 0; m_airbrushClock >= kAirbrushPeriod && i < kMaxStampsPerFrame; ++i)
            {
                m_airbrushClock -= kAirbrushPeriod;
                // Airbrush stamps deposit strength x period: strength reads as COVERAGE PER
                // SECOND of hover (a gentle flow you steer with time), not per stamp - 20
                // per-stamp-strength deposits a second saturated instantly at any setting.
                ApplyStamp(pick.uvX, pick.uvY, pick, m_strength * kAirbrushPeriod);
            }
        }
    }

    void TerrainSplatTool::ApplyStamp(f32 uvX, f32 uvY, const Pick& pick, f32 amount)
    {
        if (m_strokeWeights.Get() == nullptr)
        {
            return;
        }
        // Per-axis UV radii keep the brush a CIRCLE in world space on a non-square footprint.
        const f32 uvRadiusX = m_radius / pick.worldSizeX;
        const f32 uvRadiusY = m_radius / pick.worldSizeY;
        // `amount` is the per-stamp coverage fraction (movement/press stamps pass strength;
        // airbrush passes strength x period = coverage/second). The brush's flat CORE scales
        // with it: a 1.0 stamp keeps the decisive half-radius core (one-hot paint, hard erase),
        // a low-amount blending stamp is nearly pure cosine - a SOFT brush, so edge blends
        // grade across the whole radius instead of converging a flat core against a thin skirt.
        const f32 t = Clamp(amount, 0.0f, 1.0f);
        const f32 core = 0.5f * t;
        const terrain::SplatRegion r =
            m_smooth
                ? terrain::SmoothTopK(*m_strokeWeights, uvX, uvY, uvRadiusX, uvRadiusY, t, core)
            : m_erase
                ? terrain::EraseTopK(*m_strokeWeights, uvX, uvY, uvRadiusX, uvRadiusY, t, core)
                : terrain::PaintTopK(*m_strokeWeights, uvX, uvY, uvRadiusX, uvRadiusY,
                                     m_paletteIndex, t, core);
        if (!r.IsEmpty())
        {
            m_region.Add(r.minX, r.minY);
            m_region.Add(r.maxX, r.maxY);
        }
    }

    void TerrainSplatTool::EndStroke()
    {
        const bool hadRegion =
            m_stroking && m_strokeWeights.Get() != nullptr && !m_region.IsEmpty();
        if (hadRegion)
        {
            const i32 rasterW = m_strokeWeights->Width();
            Array<u8> beforeW = SliceRegion(
                Span<const u8>{m_beforeWeights.Data(), m_beforeWeights.Size()}, rasterW, m_region);
            Array<u8> afterW = SliceRegion(m_strokeWeights->Weights(), rasterW, m_region);
            Array<u8> beforeI = SliceRegion(
                Span<const u8>{m_beforeIndices.Data(), m_beforeIndices.Size()}, rasterW, m_region);
            Array<u8> afterI = SliceRegion(m_strokeWeights->Indices(), rasterW, m_region);
            m_commands->Execute(UniquePtr<IEditorCommand>(
                editor::EditorRootAllocator().New<SplatStrokeCommand>(m_strokeWeights, m_region,
                                                           Move(beforeW), Move(afterW),
                                                           Move(beforeI), Move(afterI)),
                editor::EditorRootAllocator()));

            // Register the write-back-to-source persist closure (drained on Save). It rewrites the
            // SOURCE SplatmapAsset envelope (dims synced, fileName cleared - an imported asset
            // converts to embedded, the editable-source convention) then writes BOTH sidecars.
            if (m_assetEdits != nullptr && !m_strokeWeightsId.IsNil())
            {
                RefPtr<terrain::SplatWeights> weights = m_strokeWeights;
                const Guid id = m_strokeWeightsId;
                m_assetEdits->RegisterAssetEdit(
                    id,
                    [weights, id](content::ContentDatabase& db) -> Status
                    {
                        content::Instance* inst = db.GetInstance(id);
                        if (inst == nullptr || weights.Get() == nullptr)
                        {
                            return Status{ErrorCode::NotFound};
                        }
                        RefPtr<ISerializable> object = inst->ReadObject();
                        auto* asset = Cast<pipeline::SplatmapAsset>(object.Get());
                        if (asset == nullptr)
                        {
                            return Status{ErrorCode::InvalidArgument}; // not a splatmap asset
                        }
                        asset->fileName = {};
                        asset->width = weights->Width();
                        asset->height = weights->Height();
                        const Status wrote = inst->WriteObject(*asset);
                        if (!wrote.IsOk())
                        {
                            return wrote;
                        }
                        const Status wroteWeights =
                            inst->WriteData(terrain::kSplatStream,
                                            terrain::SplatWeightsSource::WeightBlob(*weights));
                        if (!wroteWeights.IsOk())
                        {
                            return wroteWeights;
                        }
                        return inst->WriteData(terrain::kSplatIndexStream,
                                               terrain::SplatWeightsSource::IndexBlob(*weights));
                    });
            }
        }

        m_stroking = false;
        m_strokeWeights = nullptr;
        m_beforeWeights.Clear();
        m_beforeIndices.Clear();
        m_region = terrain::SplatRegion{};
    }

    void TerrainSplatTool::OnDeactivate()
    {
        if (m_stroking)
        {
            EndStroke(); // gesture-end guarantee: never leave a half-open stroke on a tool switch
        }
        m_hasHover = false;
    }

    void TerrainSplatTool::Draw(render::debug::DebugDraw& drawList)
    {
        if (!m_hasHover)
        {
            return;
        }
        // Cursor tint: a stable per-palette-index hue so the active layer reads at a glance;
        // the eraser rings in white, smooth in a cool grey-blue.
        Color ring{0.95f, 0.95f, 0.95f, 1.0f};
        if (m_smooth)
        {
            ring = Color{0.55f, 0.75f, 0.95f, 1.0f};
        }
        else if (!m_erase)
        {
            const f32 hue = static_cast<f32>((m_paletteIndex * 47u) % 360u) / 360.0f;
            const f32 h6 = hue * 6.0f;
            const f32 x = 1.0f - Abs(h6 - static_cast<f32>(2 * (static_cast<i32>(h6) / 2)) - 1.0f);
            const f32 comp[3][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}};
            const i32 seg = Min(static_cast<i32>(h6) / 2, 2);
            ring = Color{0.3f + 0.7f * comp[seg][0], 0.3f + 0.7f * comp[seg][1],
                         0.3f + 0.7f * comp[seg][2], 1.0f};
        }
        drawList.DrawCircleNormal(m_hoverWorld, m_radius, m_hoverNormal, ring, 40, true);
        drawList.DrawCircleNormal(m_hoverWorld, m_radius * 0.5f, m_hoverNormal,
                                  Color{ring.r, ring.g, ring.b, 0.5f}, 32, true);
    }

    void TerrainSplatTool::UpdateStatus()
    {
        m_status =
            m_smooth
                ? Format(u8"Paint Layers [SMOOTH]  radius {}  (1-9 layer, 0 eraser, - smooth, Shift+wheel size)",
                         static_cast<i32>(m_radius + 0.5f))
            : m_erase
                ? Format(u8"Paint Layers [ERASER]  radius {}  (1-9 layer, 0 eraser, - smooth, Shift+wheel size)",
                         static_cast<i32>(m_radius + 0.5f))
                : Format(u8"Paint Layers [layer {}]  radius {}  (1-9 layer, 0 eraser, - smooth, Shift+wheel size)",
                         static_cast<i32>(m_paletteIndex) + 1, // from 1, as its key and the panel
                         static_cast<i32>(m_radius + 0.5f));
    }
}
