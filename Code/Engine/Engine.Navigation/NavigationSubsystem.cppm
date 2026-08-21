// Engine::Navigation - :subsystem partition.
//
// NavigationSceneSystem: the per-scene navigation runtime. At OnSceneStarted it loads every
// NavMeshZoneComponent's cooked navmesh into a live zone (one dtCrowd + query each) and registers
// each NavAgentComponent with the zone whose AABB contains it. Each Update it applies pending
// navigate/stop requests, steps every crowd, and (for MoveEntity agents) writes the steered
// position back to the entity transform. Zones bake in ZONE-LOCAL space, so agent positions and
// targets transform through the zone entity's world matrix on the way in and out.
//
// Simulation-only: it does nothing in edit mode (IsSimulationOnly()).

module;
#include "Core/Prelude.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (compiles to nothing when disabled)

export module engine.navigation:subsystem;

import foundation.core;
import foundation.runtime;
import foundation.profiler;
import foundation.scene;
import engine.scene;
import foundation.navigation;
import foundation.navigation.resource;
import :components;

using namespace foundation::core;

export namespace engine::navigation
{
    namespace scene = foundation::scene;

    class NavigationSceneSystem final : public scene::SceneSystem
    {
    public:
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }
        // Gameplay/AI phase: crowds step before animation + render extraction consume transforms.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -100; }

        // Per-scene settings block (debug-draw flags). Reflected + serialized like physics.
        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override
        {
            return &TypeOf<NavigationSceneSettings>();
        }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &m_settings; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"navigation"; }
        void SerializeSettings(ISerializer& ar) override
        {
            SerializeNavigationSceneSettings(ar, m_settings);
        }
        [[nodiscard]] NavigationSceneSettings& Settings() noexcept { return m_settings; }

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }

        void OnSceneStarted() override
        {
            if (m_scene == nullptr)
            {
                return;
            }
            PROFILE_SCOPE("Navigation.Build"); // zone load + crowd/query construction (one-time)
            BuildZones();
            RegisterAgents();
        }

        void OnSceneStopped() override
        {
            m_zones.Clear();
            if (m_scene != nullptr)
            {
                if (auto* agents = m_scene->GetSystem<NavAgentComponentManager>())
                {
                    agents->ForEach([](NavAgentComponent& a, scene::EntityHandle)
                                    {
                                        a.agentId = -1;
                                        a.zoneIndex = -1;
                                        a.hasTarget = false;
                                        a.finished = true;
                                    });
                }
                if (auto* zones = m_scene->GetSystem<NavMeshZoneComponentManager>())
                {
                    zones->ForEach([](NavMeshZoneComponent& z, scene::EntityHandle)
                                   { z.runtimeIndex = -1; });
                }
            }
        }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::Update || m_scene == nullptr || m_zones.IsEmpty())
            {
                return;
            }
            auto* agents = m_scene->GetSystem<NavAgentComponentManager>();
            if (agents == nullptr)
            {
                return;
            }
            PROFILE_SCOPE("Navigation.Update");

            // 1. Apply pending navigate()/stop() requests (crowd works in zone-local space).
            agents->ForEach(
                [this](NavAgentComponent& a, scene::EntityHandle)
                {
                    if (a.zoneIndex < 0 || a.agentId < 0)
                    {
                        return;
                    }
                    RuntimeZone& rz = m_zones[static_cast<usize>(a.zoneIndex)];
                    if (a.targetDirty)
                    {
                        const Float3 local = TransformPoint(a.target, rz.invWorld);
                        (void)rz.crowd->SetTarget(a.agentId, local);
                        a.targetDirty = false;
                    }
                    if (a.stopRequested)
                    {
                        rz.crowd->ClearTarget(a.agentId);
                        a.stopRequested = false;
                    }
                });

            // 2. Step every crowd.
            {
                PROFILE_SCOPE("Navigation.Crowd");
                for (RuntimeZone& rz : m_zones)
                {
                    rz.crowd->Update(deltaTime);
                }
            }

            // 3. Read back: world position + velocity, transform writeback, status.
            agents->ForEach(
                [this](NavAgentComponent& a, scene::EntityHandle entity)
                {
                    if (a.zoneIndex < 0 || a.agentId < 0)
                    {
                        return;
                    }
                    RuntimeZone& rz = m_zones[static_cast<usize>(a.zoneIndex)];
                    const Float3 localPos = rz.crowd->AgentPosition(a.agentId);
                    const Float3 localVel = rz.crowd->AgentVelocity(a.agentId);
                    const Float3 worldPos = TransformPoint(localPos, rz.world);
                    a.desiredVelocity = TransformDirection(localVel, rz.world);

                    if (a.moveEntity && a.hasTarget)
                    {
                        // NOTE (P1): assumes an unparented agent (local transform == world). A
                        // parented agent would need a world->parent-local conversion.
                        Transform t = m_scene->GetLocalTransform(entity);
                        t.position = worldPos;
                        m_scene->SetLocalTransform(entity, t);
                    }

                    if (a.hasTarget)
                    {
                        const f32 dx = worldPos.x - a.target.x;
                        const f32 dz = worldPos.z - a.target.z;
                        a.remainingDistance = Sqrt(dx * dx + dz * dz);
                        a.finished = a.remainingDistance < (a.radius + 0.1f);
                    }
                });
        }

    private:
        static constexpr i32 kMaxAgentsPerZone = 128;

        struct RuntimeZone
        {
            UniquePtr<nav::NavigationCrowd> crowd;
            UniquePtr<nav::NavigationMeshQuery> query;
            Float4x4 world = Float4x4::Identity();
            Float4x4 invWorld = Float4x4::Identity();
            Float3 center{0, 0, 0}; // zone AABB center in world space
            Float3 extents{0, 0, 0};
        };

        void BuildZones()
        {
            auto* zones = m_scene->GetSystem<NavMeshZoneComponentManager>();
            if (zones == nullptr)
            {
                return;
            }
            zones->ForEach(
                [this](NavMeshZoneComponent& z, scene::EntityHandle entity)
                {
                    nav::NavigationZoneResource* product = z.zone.Get();
                    if (product == nullptr || !product->IsValid())
                    {
                        z.runtimeIndex = -1;
                        return;
                    }
                    RuntimeZone rz;
                    rz.world = m_scene->GetWorldMatrix(entity);
                    rz.invWorld = Inverse(rz.world);
                    rz.center = m_scene->GetWorldPosition(entity);
                    rz.extents = z.extents;
                    const f32 radius =
                        product->mesh.BakedAgentRadius() > 0.0f ? product->mesh.BakedAgentRadius()
                                                               : 0.6f;
                    rz.crowd = MakeUnique<nav::NavigationCrowd>(DefaultAllocator(), product->mesh,
                                                               kMaxAgentsPerZone, radius);
                    rz.query =
                        MakeUnique<nav::NavigationMeshQuery>(DefaultAllocator(), product->mesh);
                    z.runtimeIndex = static_cast<i32>(m_zones.Size());
                    m_zones.PushBack(Move(rz));
                });
        }

        void RegisterAgents()
        {
            auto* agents = m_scene->GetSystem<NavAgentComponentManager>();
            if (agents == nullptr)
            {
                return;
            }
            agents->ForEach(
                [this](NavAgentComponent& a, scene::EntityHandle entity)
                {
                    const Float3 worldPos = m_scene->GetWorldPosition(entity);
                    a.zoneIndex = FindZoneContaining(worldPos);
                    a.agentId = -1;
                    if (a.zoneIndex < 0)
                    {
                        return;
                    }
                    RuntimeZone& rz = m_zones[static_cast<usize>(a.zoneIndex)];
                    const Float3 local = TransformPoint(worldPos, rz.invWorld);
                    nav::NavigationAgentParams ap;
                    ap.radius = a.radius;
                    ap.height = a.height;
                    ap.maxSpeed = a.maxSpeed;
                    ap.maxAcceleration = a.maxAcceleration;
                    a.agentId = rz.crowd->AddAgent(local, ap);
                });
        }

        // First zone whose (world-space, axis-aligned from center+extents) box contains `worldPos`.
        [[nodiscard]] i32 FindZoneContaining(Float3 worldPos) const
        {
            for (usize i = 0; i < m_zones.Size(); ++i)
            {
                const RuntimeZone& rz = m_zones[i];
                if (Abs(worldPos.x - rz.center.x) <= rz.extents.x &&
                    Abs(worldPos.y - rz.center.y) <= rz.extents.y &&
                    Abs(worldPos.z - rz.center.z) <= rz.extents.z)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }

        scene::Scene* m_scene = nullptr;
        Array<RuntimeZone> m_zones;
        NavigationSceneSettings m_settings;
    };

    // Per-scene manager set (the same call SceneSurface + the runtime injection use).
    inline void AddNavigationSceneManagers(scene::Scene& scene)
    {
        scene.AddSystem<NavMeshZoneComponentManager>();
        scene.AddSystem<NavAgentComponentManager>();
        scene.AddSystem<NavigationSceneSystem>();
    }

    // Runtime subsystem: the DefaultApp-registered broker that injects the per-scene navigation
    // managers into every scene (via ISceneAware) and registers the component reflection once.
    // The per-scene tick lives in NavigationSceneSystem; this type owns no cross-scene state.
    class NavigationSubsystem final : public foundation::runtime::Subsystem,
                                      public scene::ISceneAware,
                                      public scene::ISceneObserver
    {
    public:
        // Assembly (a navigation scene's managers). The reactive cross-scene tracking now rides the
        // observer stages below, so scratch/headless scenes assemble without any subsystem wiring.
        void OnSceneCreated(scene::Scene& scene) override { AddNavigationSceneManagers(scene); }

        // Reactive: track the scene's NavigationSceneSystem for the debug-draw scan.
        void OnSystemsReady(scene::Scene& scene) override
        {
            m_scenes.PushBack(SceneEntry{&scene, scene.GetSystem<NavigationSceneSystem>()});
        }
        void OnDestroying(scene::Scene& scene) override
        {
            for (usize i = 0; i < m_scenes.Size(); ++i)
            {
                if (m_scenes[i].scene == &scene)
                {
                    m_scenes.RemoveAt(i);
                    return;
                }
            }
        }

        // Persistent debug draw (physics precedent): when a scene's NavigationSceneSettings.debugDraw
        // is on, draw its loaded navmesh (+ agent paths) into the render debug scene each frame.
        // Defined in the implementation unit (the render dependency stays out of this interface).
        void Update(f32 deltaTime) override;

    protected:
        void OnInit() override { RegisterNavigationComponentReflection(); }
        void OnReady() override
        {
            if (foundation::runtime::Context* context = GetContext())
            {
                if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
                {
                    scenes->RegisterSceneAware(this);
                    scenes->RegisterObserver(this, scene::SceneLifecycleStage::SystemsReady);
                    scenes->RegisterObserver(this, scene::SceneLifecycleStage::Destroying);
                }
            }
        }
        void OnShutdown() override
        {
            if (foundation::runtime::Context* context = GetContext())
            {
                if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
                {
                    scenes->UnregisterSceneAware(this);
                    scenes->UnregisterObserver(this);
                }
            }
        }

        struct SceneEntry
        {
            scene::Scene* scene = nullptr;
            NavigationSceneSystem* system = nullptr;
        };
        [[nodiscard]] Span<const SceneEntry> Scenes() const noexcept
        {
            return Span<const SceneEntry>{m_scenes.Data(), m_scenes.Size()};
        }

    private:
        Array<SceneEntry> m_scenes;
    };
}
