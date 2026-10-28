// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Vegetation - :scatter implementation (VegetationScatterTool).

module;
#include "Core/Prelude.h"

module editor.vegetation;

import foundation.core;
import foundation.scene;
import foundation.render;     // debug::DebugDraw
import foundation.shell;      // IKeyboard (layer hotkeys)
import foundation.geometry;   // StaticMesh (bounds -> the spacing reach)
import foundation.heightfield;
import foundation.physics;    // PhysicsWorld::ShapeOverlap (collision rejection)
import foundation.vegetation; // ScatterStamp, EraseInstancesInDisc
import engine.physics;        // PhysicsSceneSystem (the scene's world), UnpackEntity
import engine.vegetation;     // TerrainVegetationComponent(Manager)
import editor.core;
import editor.viewporttools;

using namespace foundation::core;

namespace editor
{
    namespace scene = foundation::scene;
    namespace render = foundation::render;
    namespace veg = foundation::vegetation;
    using engine::vegetation::TerrainVegetationComponent;
    using engine::vegetation::TerrainVegetationComponentManager;
    using engine::vegetation::PropVegetationLayer;

    namespace
    {
        [[nodiscard]] TerrainVegetationComponentManager* VegetationManagerOf(scene::Scene& scene)
        {
            scene::ComponentManagerBase* base =
                scene.FindManagerByComponentType(TypeOf<TerrainVegetationComponent>());
            return static_cast<TerrainVegetationComponentManager*>(base);
        }

        // The stroke command: the layer's whole instance list before / after (props are few).
        // Re-resolves the component each time (pools move); the manager re-buckets on the
        // content hash. One per stroke; never merges.
        class ScatterStrokeCommand final : public IEditorCommand
        {
        public:
            ScatterStrokeCommand(scene::Scene& scene, scene::EntityHandle owner, u32 layer,
                                 Array<Float4x4> before, Array<Float4x4> after)
                : m_scene(&scene), m_owner(owner), m_layer(layer), m_before(Move(before)),
                  m_after(Move(after))
            {
            }
            [[nodiscard]] bool Execute() override { return Write(m_after); }
            void Undo() override { (void)Write(m_before); }
            [[nodiscard]] StringView TypeId() const override { return u8"vegetation.scatter.stroke"; }

        private:
            bool Write(const Array<Float4x4>& instances)
            {
                TerrainVegetationComponentManager* mgr = VegetationManagerOf(*m_scene);
                TerrainVegetationComponent* c = mgr != nullptr ? mgr->Get(m_owner) : nullptr;
                if (c == nullptr || m_layer >= c->propLayers.Size())
                {
                    return false;
                }
                c->propLayers[m_layer].instances = instances;
                return true;
            }

            scene::Scene* m_scene;
            scene::EntityHandle m_owner;
            u32 m_layer;
            Array<Float4x4> m_before;
            Array<Float4x4> m_after;
        };

        // The scene's physics world, when the scene has one.
        [[nodiscard]] foundation::physics::PhysicsWorld* PhysicsWorldOf(scene::Scene& scene)
        {
            auto* system = scene.GetSystem<engine::physics::PhysicsSceneSystem>();
            return system != nullptr ? system->World() : nullptr;
        }
    }

    struct VegetationScatterTool::Pick
    {
        VegetationPick fp;
        PropVegetationLayer* layer = nullptr;
    };

    bool VegetationScatterTool::IsAvailable() const
    {
        TerrainVegetationComponentManager* mgr = VegetationManagerOf(*m_scene);
        if (mgr == nullptr || !VegetationPick::AnyFootprint(*m_scene, /*requireMask*/ false))
        {
            return false;
        }
        bool any = false;
        mgr->ForEach([&](TerrainVegetationComponent& c, scene::EntityHandle)
                     { any |= !c.propLayers.IsEmpty(); });
        return any;
    }

    bool VegetationScatterTool::Update(const ViewportToolInput& input)
    {
        m_hasHover = false;
        if (foundation::shell::IKeyboard* kb = input.keyboard; kb != nullptr && input.pointerValid)
        {
            using foundation::shell::KeyCode;
            if (kb->IsKeyPressed(KeyCode::Num1)) SetLayer(0);
            if (kb->IsKeyPressed(KeyCode::Num2)) SetLayer(1);
            if (kb->IsKeyPressed(KeyCode::Num3)) SetLayer(2);
            if (kb->IsKeyPressed(KeyCode::Num4)) SetLayer(3);
            if (kb->IsKeyPressed(KeyCode::Num5)) SetLayer(4);
            if (kb->IsKeyPressed(KeyCode::Num6)) SetLayer(5);
            if (kb->IsKeyPressed(KeyCode::Num7)) SetLayer(6);
            if (kb->IsKeyPressed(KeyCode::Num8)) SetLayer(7);
            if (kb->IsKeyPressed(KeyCode::Num9)) SetLayer(8);
            if (kb->IsKeyPressed(KeyCode::Num0)) SetEraser(true);
        }
        // Shift + wheel resizes the brush; the bare wheel stays the camera's dolly (2026-09-23).
        if (input.pointerOver && input.shift && input.wheelDelta != 0.0f)
        {
            SetRadius(m_radius * (1.0f + 0.12f * input.wheelDelta));
        }
        // The prop brush picks the terrain PLANE, through a cut (the stamp itself refuses a cut
        // cell; the eraser must reach the props left standing over one - Specs/terrain-holes.md).
        const VegetationPick fp = VegetationPick::Resolve(*m_scene, input.ray.origin, input.ray.direction,
                                                          /*requireMask*/ false, /*ignoreHoles*/ true);
        if (fp.valid)
        {
            m_hasHover = true;
            m_hoverWorld = fp.worldHit;
            m_hoverNormal = fp.worldNormal;
            m_layerHasMesh = fp.component == nullptr || m_layer >= fp.component->propLayers.Size() ||
                             fp.component->propLayers[m_layer].mesh.Get() != nullptr;
        }
        bool consumed = m_stroking;
        if (!input.editingLocked) // Simulate: the props feed the live scene - no edits
        {
            const bool paintable =
                fp.valid && fp.component != nullptr && m_layer < fp.component->propLayers.Size();
            if (!m_stroking && paintable && input.pointerOver && input.leftPressed)
            {
                BeginStroke(input);
                consumed = true;
            }
            else if (m_stroking && input.leftDown && fp.valid && fp.owner == m_strokeOwner)
            {
                AdvanceStroke(input);
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

    void VegetationScatterTool::BeginStroke(const ViewportToolInput& input)
    {
        const VegetationPick fp = VegetationPick::Resolve(*m_scene, input.ray.origin, input.ray.direction,
                                                          /*requireMask*/ false, /*ignoreHoles*/ true);
        if (!fp.valid || fp.component == nullptr || m_layer >= fp.component->propLayers.Size())
        {
            return;
        }
        m_stroking = true;
        m_strokeOwner = fp.owner;
        m_strokeTerrain = fp.terrainEntity;
        m_strokeLayer = m_layer;
        m_strokeStamps = 0;
        m_before = fp.component->propLayers[m_layer].instances;
        ApplyStamp(fp.localHit, input);
        m_lastStampLocal = fp.localHit;
    }

    // Distance-spaced stamps along the drag (the paint brushes' walker): a stamp every
    // m_stampSpacing x radius of terrain-local travel, so a fast drag leaves no gaps.
    void VegetationScatterTool::AdvanceStroke(const ViewportToolInput& input)
    {
        const VegetationPick fp = VegetationPick::Resolve(*m_scene, input.ray.origin, input.ray.direction,
                                                          /*requireMask*/ false, /*ignoreHoles*/ true);
        if (!fp.valid || fp.owner != m_strokeOwner)
        {
            return;
        }
        const f32 spacing = Max(m_radius * m_stampSpacing, 1e-3f);
        for (i32 guard = 0; guard < 1024; ++guard)
        {
            const f32 dx = fp.localHit.x - m_lastStampLocal.x;
            const f32 dz = fp.localHit.z - m_lastStampLocal.z;
            const f32 dist = Sqrt(dx * dx + dz * dz);
            if (dist < spacing)
            {
                break;
            }
            const f32 f = spacing / dist;
            m_lastStampLocal.x += dx * f;
            m_lastStampLocal.z += dz * f;
            m_lastStampLocal.y = fp.heightfield->GetHeightAt(m_lastStampLocal.x, m_lastStampLocal.z);
            ApplyStamp(m_lastStampLocal, input);
        }
    }

    void VegetationScatterTool::ApplyStamp(Float3 localCentre, const ViewportToolInput& input)
    {
        const VegetationPick fp = VegetationPick::Resolve(*m_scene, input.ray.origin, input.ray.direction,
                                                          /*requireMask*/ false, /*ignoreHoles*/ true);
        if (!fp.valid || fp.component == nullptr || m_strokeLayer >= fp.component->propLayers.Size())
        {
            return;
        }
        PropVegetationLayer& layer = fp.component->propLayers[m_strokeLayer];
        if (m_erase)
        {
            (void)veg::EraseInstancesInDisc(layer.instances, localCentre.x, localCentre.z, m_radius);
            ++m_strokeStamps;
            return;
        }
        // The collision query: the override (tests, hosts), else the scene's physics world with
        // the terrain's own body excluded (every stamp sits on it), else nothing.
        veg::BlockedQuery blocked;
        const Float4x4 terrainWorld = fp.terrainWorld;
        if (m_blockedOverride)
        {
            const BlockedQuery& query = m_blockedOverride;
            blocked = [&query, terrainWorld](Float3 local, f32 radius)
            { return query(TransformPoint(local, terrainWorld), radius); };
        }
        else if (foundation::physics::PhysicsWorld* world = PhysicsWorldOf(*m_scene))
        {
            const scene::EntityHandle terrain = fp.terrainEntity;
            const scene::EntityHandle owner = fp.owner;
            blocked = [world, terrainWorld, terrain, owner](Float3 local, f32 radius)
            {
                foundation::physics::QueryShape sphere;
                sphere.kind = foundation::physics::ShapeKind::Sphere;
                sphere.radius = radius;
                const Float3 centre = TransformPoint(local, terrainWorld) + Float3{0.0f, radius, 0.0f};
                Array<foundation::physics::BodyId> bodies;
                world->ShapeOverlap(sphere, centre, Quaternion::Identity, bodies);
                for (foundation::physics::BodyId body : bodies)
                {
                    const scene::EntityHandle e = engine::physics::UnpackEntity(world->UserData(body));
                    if (e != terrain && e != owner)
                    {
                        return true; // inside (or against) another body
                    }
                }
                return false;
            };
        }
        const AABB meshBounds = layer.mesh.Get() != nullptr ? layer.mesh.Get()->bounds : AABB::Empty();
        const u64 seed = HashBytes(&m_strokeStamps, sizeof(m_strokeStamps),
                                   HashBytes(&m_strokeCount, sizeof(m_strokeCount), 0x5eedu));
        Array<Float4x4> placed;
        (void)veg::ScatterStamp(seed, *fp.heightfield, layer.ToScatterLayer(), meshBounds,
                                localCentre.x, localCentre.z, m_radius, m_density, m_strength,
                                m_spacing,
                                Span<const Float4x4>{layer.instances.Data(), layer.instances.Size()},
                                blocked, placed);
        for (const Float4x4& m : placed)
        {
            layer.instances.PushBack(m);
        }
        ++m_strokeStamps;
    }

    void VegetationScatterTool::EndStroke()
    {
        if (m_stroking)
        {
            TerrainVegetationComponentManager* mgr = VegetationManagerOf(*m_scene);
            TerrainVegetationComponent* c = mgr != nullptr ? mgr->Get(m_strokeOwner) : nullptr;
            if (c != nullptr && m_strokeLayer < c->propLayers.Size())
            {
                const Array<Float4x4>& after = c->propLayers[m_strokeLayer].instances;
                const bool changed = after.Size() != m_before.Size() ||
                                     (!after.IsEmpty() &&
                                      MemCompare(after.Data(), m_before.Data(),
                                                 after.Size() * sizeof(Float4x4)) != 0);
                if (changed)
                {
                    Array<Float4x4> afterCopy = after;
                    m_commands->Execute(UniquePtr<IEditorCommand>(
                        editor::EditorRootAllocator().New<ScatterStrokeCommand>(
                            *m_scene, m_strokeOwner, m_strokeLayer, Move(m_before), Move(afterCopy)),
                        editor::EditorRootAllocator()));
                }
            }
            ++m_strokeCount;
        }
        m_stroking = false;
        m_before.Clear();
    }

    void VegetationScatterTool::OnDeactivate()
    {
        if (m_stroking)
        {
            EndStroke();
        }
        m_hasHover = false;
    }

    void VegetationScatterTool::Draw(render::debug::DebugDraw& drawList)
    {
        if (!m_hasHover)
        {
            return;
        }
        const Color ring = m_erase ? Color{0.95f, 0.95f, 0.95f, 1.0f} : Color{0.85f, 0.6f, 0.2f, 1.0f};
        drawList.DrawCircleNormal(m_hoverWorld, m_radius, m_hoverNormal, ring, 40, true);
        drawList.DrawCircleNormal(m_hoverWorld, m_radius * 0.5f, m_hoverNormal,
                                  Color{ring.r, ring.g, ring.b, 0.5f}, 32, true);
    }

    void VegetationScatterTool::UpdateStatus()
    {
        const i32 radius = static_cast<i32>(m_radius + 0.5f);
        m_status = Format(u8"Paint Props [{} layer {}]  radius {}  (1-9 layer, 0 erase, Shift+wheel size)",
                          m_erase ? StringView(u8"ERASE") : StringView(u8"paint"),
                          static_cast<i32>(m_layer) + 1, radius); // from 1, as its key and the panel
        if (!m_erase && !m_layerHasMesh)
        {
            m_status = Format(u8"{}  - layer {} has no mesh: props place but nothing draws",
                              m_status.AsView(), static_cast<i32>(m_layer) + 1);
        }
    }
}
