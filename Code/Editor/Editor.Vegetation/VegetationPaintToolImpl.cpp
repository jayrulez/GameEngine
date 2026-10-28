// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Vegetation - :paint implementation (VegetationPaintTool + the viewport-tool provider).
//
// The heavy engine/resource imports the interface partition keeps out (GCC hygiene) live here:
// the vegetation + terrain component managers (to find the mask and the heightfield the pick
// needs) and the mask source + asset (to serialize the painted planes back on save).

module;
#include "Core/Prelude.h"

module editor.vegetation;

import foundation.core;
import foundation.scene;
import foundation.render;              // debug::DebugDraw
import foundation.shell;               // IKeyboard (plane hotkeys)
import foundation.content;             // ContentDatabase, Instance (the persist closure)
import foundation.resource;            // Ref<>
import foundation.vegetation.resource; // VegetationMask + PaintMask/EraseMask/SmoothMask + source
import vegetation.pipeline;            // VegetationMaskAsset (the SOURCE envelope the persist rewrites)
import engine.vegetation;              // TerrainVegetationComponent(Manager): the mask + regrow notices
import editor.core;
import editor.viewporttools;

using namespace foundation::core;

namespace editor
{
    namespace scene = foundation::scene;
    namespace render = foundation::render;
    namespace content = foundation::content;
    namespace veg = foundation::vegetation;
    using engine::vegetation::TerrainVegetationComponent;
    using engine::vegetation::TerrainVegetationComponentManager;

    namespace
    {
        // Tell the manager which footprint rect a texel region of the mask covers.
        void NotifyRegion(TerrainVegetationComponentManager* manager, const veg::VegetationMask& mask,
                          const veg::MaskRegion& region, i32 gridSize)
        {
            if (manager == nullptr || region.IsEmpty() || mask.IsEmpty())
            {
                return;
            }
            const f32 w = static_cast<f32>(mask.Width());
            const f32 h = static_cast<f32>(mask.Height());
            manager->InvalidateFootprint(static_cast<f32>(region.minX) / w,
                                         static_cast<f32>(region.minY) / h,
                                         static_cast<f32>(region.maxX + 1) / w,
                                         static_cast<f32>(region.maxY + 1) / h, gridSize);
        }

        // The region-delta stroke command over ONE plane: replays the touched rectangle between
        // its before / after blocks, bumps the mask version and tells the manager the rect changed.
        // One per stroke; never merges. Keeps the raster alive.
        class VegetationStrokeCommand final : public IEditorCommand
        {
        public:
            VegetationStrokeCommand(RefPtr<veg::VegetationMask> mask, u32 plane, veg::MaskRegion region,
                              Array<u8> before, Array<u8> after,
                              TerrainVegetationComponentManager* manager, i32 gridSize)
                : m_mask(Move(mask)), m_plane(plane), m_region(region), m_before(Move(before)),
                  m_after(Move(after)), m_manager(manager), m_gridSize(gridSize)
            {
            }
            [[nodiscard]] bool Execute() override
            {
                Write(m_after); // the live plane is already AFTER on push; replays on redo
                return !m_region.IsEmpty();
            }
            void Undo() override { Write(m_before); }
            [[nodiscard]] StringView TypeId() const override { return u8"vegetation.paint.stroke"; }

        private:
            void Write(const Array<u8>& block)
            {
                if (m_mask.Get() == nullptr || m_region.IsEmpty())
                {
                    return;
                }
                Span<u8> plane = m_mask->Plane(m_plane);
                if (plane.IsEmpty())
                {
                    return;
                }
                const i32 w = m_region.Width();
                const i32 rasterW = m_mask->Width();
                for (i32 y = 0; y < m_region.Height(); ++y)
                {
                    for (i32 x = 0; x < w; ++x)
                    {
                        plane[static_cast<usize>(m_region.minY + y) * static_cast<usize>(rasterW) +
                              static_cast<usize>(m_region.minX + x)] =
                            block[static_cast<usize>(y) * static_cast<usize>(w) +
                                  static_cast<usize>(x)];
                    }
                }
                m_mask->BumpVersion();
                NotifyRegion(m_manager, *m_mask, m_region, m_gridSize);
            }

            RefPtr<veg::VegetationMask> m_mask;
            u32 m_plane;
            veg::MaskRegion m_region;
            Array<u8> m_before;
            Array<u8> m_after;
            TerrainVegetationComponentManager* m_manager; // borrowed (the scene outlives its commands)
            i32 m_gridSize;
        };

        // Slice the inclusive texel region out of a full plane.
        [[nodiscard]] Array<u8> SliceRegion(Span<const u8> plane, i32 rasterW,
                                            const veg::MaskRegion& r)
        {
            Array<u8> out;
            if (r.IsEmpty())
            {
                return out;
            }
            const i32 w = r.Width();
            const i32 h = r.Height();
            out.Resize(static_cast<usize>(w) * static_cast<usize>(h));
            for (i32 y = 0; y < h; ++y)
            {
                for (i32 x = 0; x < w; ++x)
                {
                    out[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)] =
                        plane[static_cast<usize>(r.minY + y) * static_cast<usize>(rasterW) +
                              static_cast<usize>(r.minX + x)];
                }
            }
            return out;
        }

        constexpr i32 kMaxStampsPerFrame = 1024; // teleport guard (alt-tab warps, huge drags)
        constexpr f32 kAirbrushPeriod = 0.05f;   // seconds per time-cadence stamp (20/s)
    }

    bool VegetationPaintTool::IsAvailable() const
    {
        return VegetationPick::AnyFootprint(*m_scene, /*requireMask*/ true);
    }

    VegetationPaintTool::Pick VegetationPaintTool::ResolvePick(const ViewportToolInput& input) const
    {
        Pick pick;
        const VegetationPick fp =
            VegetationPick::Resolve(*m_scene, input.ray.origin, input.ray.direction, /*requireMask*/ true);
        if (!fp.valid || fp.mask == nullptr)
        {
            return pick;
        }
        pick.mask = fp.mask;
        pick.maskId = fp.maskId;
        pick.manager = fp.manager;
        pick.gridSize = fp.gridSize;
        pick.uvX = fp.uvX;
        pick.uvY = fp.uvY;
        pick.worldSizeX = fp.worldSizeX;
        pick.worldSizeY = fp.worldSizeY;
        pick.worldHit = fp.worldHit;
        pick.worldNormal = fp.worldNormal;
        pick.valid = true;
        return pick;
    }

    bool VegetationPaintTool::Update(const ViewportToolInput& input)
    {
        m_hasHover = false;
        // Plane hotkeys 1..9 -> planes 0..8; 0 = the ERASER; minus = SMOOTH (the splat brush's
        // keys, one convention for every terrain-footprint brush).
        if (foundation::shell::IKeyboard* kb = input.keyboard; kb != nullptr && input.pointerValid)
        {
            using foundation::shell::KeyCode;
            if (kb->IsKeyPressed(KeyCode::Num1)) SetPlane(0);
            if (kb->IsKeyPressed(KeyCode::Num2)) SetPlane(1);
            if (kb->IsKeyPressed(KeyCode::Num3)) SetPlane(2);
            if (kb->IsKeyPressed(KeyCode::Num4)) SetPlane(3);
            if (kb->IsKeyPressed(KeyCode::Num5)) SetPlane(4);
            if (kb->IsKeyPressed(KeyCode::Num6)) SetPlane(5);
            if (kb->IsKeyPressed(KeyCode::Num7)) SetPlane(6);
            if (kb->IsKeyPressed(KeyCode::Num8)) SetPlane(7);
            if (kb->IsKeyPressed(KeyCode::Num9)) SetPlane(8);
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
        if (!input.editingLocked) // Simulate: the mask feeds the live scatter - no edits
        {
            if (!m_stroking && pick.valid && input.pointerOver && input.leftPressed)
            {
                BeginStroke(pick);
                consumed = true;
            }
            else if (m_stroking && input.leftDown && pick.valid && pick.mask == m_strokeMask.Get())
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

    void VegetationPaintTool::BeginStroke(const Pick& pick)
    {
        m_stroking = true;
        m_strokeMask = RefPtr<veg::VegetationMask>(pick.mask); // keep the raster alive
        m_strokeMaskId = pick.maskId;
        m_strokeManager = pick.manager;
        m_strokeGridSize = pick.gridSize;
        const Span<const u8> plane = pick.mask->Plane(m_plane);
        m_beforePlane.Resize(plane.Size());
        if (!plane.IsEmpty())
        {
            MemCopy(m_beforePlane.Data(), plane.Data(), plane.Size());
        }
        m_region = veg::MaskRegion{};
        m_airbrushClock = 0.0f;
        // The press deposits ONE full stamp and anchors the spacing walker there.
        ApplyStamp(pick.uvX, pick.uvY, pick, m_strength);
        m_lastStampU = pick.uvX;
        m_lastStampV = pick.uvY;
    }

    // Distance-spaced stamps along the drag (+ time-cadence stamps when airbrush is on): the
    // splat brush's model - frame-rate and drag-speed independent.
    void VegetationPaintTool::AdvanceStroke(const Pick& pick, f32 deltaSeconds)
    {
        if (m_strokeMask.Get() == nullptr)
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
                break; // the remainder carries to the next frame
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
                ApplyStamp(pick.uvX, pick.uvY, pick, m_strength * kAirbrushPeriod);
            }
        }
    }

    void VegetationPaintTool::ApplyStamp(f32 uvX, f32 uvY, const Pick& pick, f32 amount)
    {
        if (m_strokeMask.Get() == nullptr)
        {
            return;
        }
        // Per-axis UV radii keep the brush a CIRCLE in world space on a non-square footprint;
        // the flat core scales with the stamp amount (soft at blending strengths, hard at 1).
        const f32 uvRadiusX = m_radius / pick.worldSizeX;
        const f32 uvRadiusY = m_radius / pick.worldSizeY;
        const f32 t = Clamp(amount, 0.0f, 1.0f);
        const f32 core = 0.5f * t;
        const veg::MaskRegion r =
            m_smooth ? veg::SmoothMask(*m_strokeMask, m_plane, uvX, uvY, uvRadiusX, uvRadiusY, t, core)
            : m_erase ? veg::EraseMask(*m_strokeMask, m_plane, uvX, uvY, uvRadiusX, uvRadiusY, t, core)
                      : veg::PaintMask(*m_strokeMask, m_plane, uvX, uvY, uvRadiusX, uvRadiusY, t, core);
        if (!r.IsEmpty())
        {
            m_region.Add(r.minX, r.minY);
            m_region.Add(r.maxX, r.maxY);
            // The live regrow: only the chunks under this stamp (the brush op bumped the version).
            NotifyRegion(static_cast<TerrainVegetationComponentManager*>(m_strokeManager),
                         *m_strokeMask, r, m_strokeGridSize);
        }
    }

    void VegetationPaintTool::EndStroke()
    {
        const bool hadRegion = m_stroking && m_strokeMask.Get() != nullptr && !m_region.IsEmpty();
        if (hadRegion)
        {
            const i32 rasterW = m_strokeMask->Width();
            Array<u8> before = SliceRegion(
                Span<const u8>{m_beforePlane.Data(), m_beforePlane.Size()}, rasterW, m_region);
            Array<u8> after = SliceRegion(m_strokeMask->Plane(m_plane), rasterW, m_region);
            m_commands->Execute(UniquePtr<IEditorCommand>(
                editor::EditorRootAllocator().New<VegetationStrokeCommand>(
                    m_strokeMask, m_plane, m_region, Move(before), Move(after),
                    static_cast<TerrainVegetationComponentManager*>(m_strokeManager),
                    m_strokeGridSize),
                editor::EditorRootAllocator()));
            // The write-back-to-source persist closure (drained on Save): rewrites the SOURCE
            // VegetationMaskAsset envelope (dims + planes synced, fileName cleared - an imported
            // mask converts to embedded) then writes the planes sidecar.
            if (m_assetEdits != nullptr && !m_strokeMaskId.IsNil())
            {
                RefPtr<veg::VegetationMask> mask = m_strokeMask;
                const Guid id = m_strokeMaskId;
                m_assetEdits->RegisterAssetEdit(
                    id,
                    [mask, id](content::ContentDatabase& db) -> Status
                    {
                        content::Instance* inst = db.GetInstance(id);
                        if (inst == nullptr || mask.Get() == nullptr)
                        {
                            return Status{ErrorCode::NotFound};
                        }
                        RefPtr<ISerializable> object = inst->ReadObject();
                        auto* asset = Cast<pipeline::VegetationMaskAsset>(object.Get());
                        if (asset == nullptr)
                        {
                            return Status{ErrorCode::InvalidArgument}; // not a mask asset
                        }
                        asset->fileName = {};
                        asset->width = mask->Width();
                        asset->height = mask->Height();
                        asset->planeCount = mask->PlaneCount();
                        const Status wrote = inst->WriteObject(*asset);
                        if (!wrote.IsOk())
                        {
                            return wrote;
                        }
                        return inst->WriteData(veg::kVegetationMaskStream,
                                               veg::VegetationMaskSource::DensityBlob(*mask));
                    });
            }
        }
        m_stroking = false;
        m_strokeMask = nullptr;
        m_strokeManager = nullptr;
        m_beforePlane.Clear();
        m_region = veg::MaskRegion{};
    }

    void VegetationPaintTool::OnDeactivate()
    {
        if (m_stroking)
        {
            EndStroke(); // never leave a half-open stroke on a tool switch
        }
        m_hasHover = false;
    }

    void VegetationPaintTool::Draw(render::debug::DebugDraw& drawList)
    {
        if (!m_hasHover)
        {
            return;
        }
        // A green ring per plane (hue steps per plane), white for the eraser, grey-blue for smooth.
        Color ring{0.95f, 0.95f, 0.95f, 1.0f};
        if (m_smooth)
        {
            ring = Color{0.55f, 0.75f, 0.95f, 1.0f};
        }
        else if (!m_erase)
        {
            const f32 shift = static_cast<f32>((m_plane * 37u) % 100u) / 100.0f;
            ring = Color{0.25f + 0.5f * shift, 0.85f, 0.25f + 0.5f * (1.0f - shift), 1.0f};
        }
        drawList.DrawCircleNormal(m_hoverWorld, m_radius, m_hoverNormal, ring, 40, true);
        drawList.DrawCircleNormal(m_hoverWorld, m_radius * 0.5f, m_hoverNormal,
                                  Color{ring.r, ring.g, ring.b, 0.5f}, 32, true);
    }

    void VegetationPaintTool::UpdateStatus()
    {
        const i32 radius = static_cast<i32>(m_radius + 0.5f);
        const StringView mode = m_smooth ? StringView(u8"SMOOTH") : m_erase ? StringView(u8"ERASE") : StringView(u8"paint");
        m_status = Format(u8"Paint Vegetation [{} plane {}]  radius {}  (1-9 plane, 0 erase, - smooth, Shift+wheel size)",
                          mode, static_cast<i32>(m_plane) + 1, radius); // from 1, as its key and the panel
    }

    void VegetationViewportToolProvider::CreateTools(ViewportToolManager& manager,
                                                     const ViewportToolHostContext& context)
    {
        if (context.scene == nullptr || context.commands == nullptr)
        {
            return;
        }
        manager.Add(UniquePtr<IViewportTool>(
            editor::EditorRootAllocator().New<VegetationPaintTool>(*context.scene, *context.commands,
                                                                   context.assetEdits),
            editor::EditorRootAllocator()));
        manager.Add(UniquePtr<IViewportTool>(
            editor::EditorRootAllocator().New<VegetationScatterTool>(*context.scene,
                                                                     *context.commands),
            editor::EditorRootAllocator()));
    }

    void RegisterVegetationViewportTools()
    {
        static VegetationViewportToolProvider provider;
        ViewportToolProviderRegistry::Get().Register(&provider);
    }
}
