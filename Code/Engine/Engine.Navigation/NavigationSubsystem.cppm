// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Navigation - :subsystem partition.
//
// NavigationSceneSystem: the per-scene navigation runtime. At OnSceneStarted it loads every
// NavMeshZoneComponent's cooked navmesh into a live zone (one dtCrowd and query each) and registers
// each NavAgentComponent with the zone whose AABB contains it. Each Update it applies pending
// navigate/stop requests, steps every crowd, and (for MoveEntity agents) writes the steered
// position back to the entity transform. Zones bake in zone-local space, so agent positions and
// targets transform through the zone entity's world matrix on the way in and out.
//
// Simulation-only: it does nothing in edit mode (IsSimulationOnly()).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE (compiles to nothing when disabled)

export module engine.navigation:subsystem;

import foundation.core;
import engine.domain;
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
        // Gameplay/AI phase: crowds step before animation and render extraction consume transforms.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -100; }

        // Per-scene settings block (debug-draw flags), reflected and serialized like physics.
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

        // The last bake's stage capture (zone-local space, and the zone entity to place it by). The
        // editor bake deposits it here; the debug overlay draws it when the settings flag is on.
        // Transient, never serialized.
        struct BakeStageCache
        {
            scene::EntityHandle zoneEntity{};
            Array<Float3> contourLines;    // segment pairs
            Array<Float3> walkableSamples; // span-top points
        };
        void SetBakeStages(scene::EntityHandle zoneEntity, Array<Float3> contourLines,
                           Array<Float3> walkableSamples)
        {
            m_bakeStages.zoneEntity = zoneEntity;
            m_bakeStages.contourLines = static_cast<Array<Float3>&&>(contourLines);
            m_bakeStages.walkableSamples = static_cast<Array<Float3>&&>(walkableSamples);
        }
        [[nodiscard]] const BakeStageCache& BakeStages() const noexcept { return m_bakeStages; }

        void OnSceneCreate(scene::Scene& scene) override
        {
            m_scene = &scene;
            // Everything this system allocates (crowds, queries, their Detour impls)
            // rolls up under the Navigation memory tag, backed by the scene's allocator.
            m_allocator = MakeUnique<TaggedAllocator>(scene.Allocator(), scene.Allocator(),
                                                      RegisterMemoryTag("Navigation"));
        }

        void OnSceneStarted() override
        {
            if (m_scene == nullptr)
            {
                return;
            }
            PROFILE_SCOPE("Navigation.Build"); // zone load and crowd/query construction (one-time)
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
                    (void)agents->TakeReleased(); // their crowds are gone with the zones
                    agents->ForEach([](NavAgentComponent& a, scene::EntityHandle)
                                    {
                                        a.agentId = -1;
                                        a.zoneIndex = -1;
                                        a.registered = false;
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

            // 0. A destroyed agent leaves its crowd (else it stands there for ever, an obstacle no
            //    one can see, holding a slot). Before any rebuild below: its slot number names a
            //    slot of the crowd it was in, and a rebuilt crowd hands that number to another.
            for (const NavAgentComponentManager::ReleasedAgent& gone : agents->TakeReleased())
            {
                if (gone.zoneIndex >= 0 && static_cast<usize>(gone.zoneIndex) < m_zones.Size())
                {
                    m_zones[static_cast<usize>(gone.zoneIndex)].crowd->RemoveAgent(gone.agentId);
                }
            }

            // A zone whose navmesh was re-baked and reloaded since its crowd was built gets a crowd
            // over the new one (the old product stays alive until then: we hold it).
            RebuildReloadedZones(*agents);

            // 1. Apply pending warp()/navigate()/stop() requests (crowd works in zone-local space).
            agents->ForEach(
                [this](NavAgentComponent& a, scene::EntityHandle entity)
                {
                    if (!a.registered)
                    {
                        Register(a, entity); // added since the start (a spawned prefab's)
                    }
                    if (a.warpRequested)
                    {
                        Warp(a, entity);
                    }
                    if (a.zoneIndex < 0 || a.agentId < 0)
                    {
                        return;
                    }
                    RuntimeZone& rz = m_zones[static_cast<usize>(a.zoneIndex)];
                    // Live steering-profile changes (setSpeed, navigateAt, an inspector edit) push
                    // into the crowd. They are value-compared, so an unchanged agent costs one
                    // float compare.
                    if (a.maxSpeed != a.appliedSpeed || a.maxAcceleration != a.appliedAcceleration ||
                        a.avoidOthers != a.appliedAvoidOthers)
                    {
                        nav::NavigationAgentParams params;
                        params.radius = a.radius;
                        params.height = a.height;
                        params.maxSpeed = a.maxSpeed;
                        params.maxAcceleration = a.maxAcceleration;
                        params.avoidOthers = a.avoidOthers;
                        rz.crowd->SetAgentParams(a.agentId, params);
                        a.appliedSpeed = a.maxSpeed;
                        a.appliedAcceleration = a.maxAcceleration;
                        a.appliedAvoidOthers = a.avoidOthers;
                    }
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
                    if (a.velocityDirty)
                    {
                        (void)rz.crowd->SetVelocity(a.agentId, TransformDirection(a.steerVelocity, rz.invWorld));
                        a.velocityDirty = false;
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

            // 3. Read back: world position and velocity, transform writeback, status.
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

                    // Introspection cache: the crowd's internals, readable from scripts and drawn
                    // by the debug overlay (why is this agent not moving).
                    const nav::NavigationAgentState st = rz.crowd->AgentState(a.agentId);
                    a.crowdState = static_cast<u8>(st.state);
                    a.crowdTargetState = static_cast<u8>(st.targetState);
                    a.crowdDesiredSpeed = st.desiredSpeed;
                    a.pathCorners = st.cornerCount;

                    if (a.moveEntity && (a.hasTarget || a.steering))
                    {
                        // Assumes an unparented agent (local transform == world). A parented agent
                        // would need a world-to-parent-local conversion.
                        Transform t = m_scene->GetLocalTransform(entity);
                        t.position = worldPos;
                        m_scene->SetLocalTransform(entity, t);
                    }

                    if (a.hasTarget)
                    {
                        const f32 dx = worldPos.x - a.target.x;
                        const f32 dz = worldPos.z - a.target.z;
                        a.remainingDistance = Sqrt(dx * dx + dz * dz);
                        const bool wasFinished = a.finished;
                        a.finished = a.remainingDistance < (a.radius + 0.1f + a.stopDistance);
                        // Arrival radius: on reaching the ring, stop steering; otherwise the crowd
                        // keeps pushing the agent onto the exact point the radius was meant to keep
                        // it away from. navigate() re-arms.
                        if (!wasFinished && a.finished && a.stopDistance > 0.0f)
                        {
                            rz.crowd->ClearTarget(a.agentId);
                        }
                    }
                });
        }

    private:
        struct RuntimeZone
        {
            // The navmesh product the crowd and query were built over, held so a reload (a re-bake
            // cooked while the scene runs) cannot free it under them; compared with what the
            // zone's ref resolves to now, to notice the reload and rebuild over the new one.
            RefPtr<nav::NavigationZoneResource> product;
            scene::EntityHandle entity{};
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
                        // Said, not silent: every agent in it would only report standing in no zone.
                        LOG_WARNING(u8"Navigation",
                                    u8"navigation zone '{}' has no usable navmesh (no asset, not baked, or it "
                                    u8"did not load); agents in it will not move",
                                    m_scene->GetEntityName(entity));
                        z.runtimeIndex = -1;
                        return;
                    }
                    RuntimeZone rz;
                    // Rigid (no scale): matches the bake frame (NavigationBakeImpl), so a scaled
                    // zone entity does not double-scale the navmesh at runtime.
                    rz.world = RigidPart(m_scene->GetWorldMatrix(entity));
                    rz.invWorld = Inverse(rz.world);
                    rz.center = m_scene->GetWorldPosition(entity);
                    rz.extents = z.extents;
                    rz.entity = entity;
                    BuildCrowd(rz, *product);
                    z.runtimeIndex = static_cast<i32>(m_zones.Size());
                    m_zones.PushBack(Move(rz));
                });
        }

        // The crowd and the query over `product`'s navmesh, which the zone then holds.
        void BuildCrowd(RuntimeZone& rz, nav::NavigationZoneResource& product)
        {
            rz.product = RefPtr<nav::NavigationZoneResource>(&product);
            const f32 radius =
                product.mesh.BakedAgentRadius() > 0.0f ? product.mesh.BakedAgentRadius() : 0.6f;
            const NavMeshZoneComponent* zone =
                m_scene != nullptr ? m_scene->GetSystem<NavMeshZoneComponentManager>()->Get(rz.entity) : nullptr;
            const i32 maxAgents = zone != nullptr && zone->maxAgents > 0 ? zone->maxAgents : 128;
            rz.crowd = MakeUnique<nav::NavigationCrowd>(*m_allocator, *m_allocator, product.mesh,
                                                        maxAgents, radius);
            rz.query = MakeUnique<nav::NavigationMeshQuery>(*m_allocator, *m_allocator, product.mesh);
        }

        // Each zone whose ref now resolves to a different, usable navmesh than its crowd was built
        // over (a re-bake reloaded while the scene runs): a new crowd and query over the new one,
        // its agents added back where they stand, their destinations sent again. A reload still
        // decoding (no product yet) keeps the old crowd running until it lands.
        void RebuildReloadedZones(NavAgentComponentManager& agents)
        {
            auto* zones = m_scene->GetSystem<NavMeshZoneComponentManager>();
            if (zones == nullptr)
            {
                return;
            }
            for (usize i = 0; i < m_zones.Size(); ++i)
            {
                RuntimeZone& rz = m_zones[i];
                const NavMeshZoneComponent* z = zones->Get(rz.entity);
                nav::NavigationZoneResource* now = z != nullptr ? z->zone.Get() : nullptr;
                if (now == nullptr || now == rz.product.Get() || !now->IsValid())
                {
                    continue;
                }
                rz.query = nullptr; // the old crowd and query go before the product they read
                rz.crowd = nullptr;
                BuildCrowd(rz, *now);
                agents.ForEach(
                    [&](NavAgentComponent& a, scene::EntityHandle entity)
                    {
                        if (a.zoneIndex != static_cast<i32>(i))
                        {
                            return;
                        }
                        a.agentId = AddToCrowd(rz, a, m_scene->GetWorldPosition(entity));
                        a.targetDirty = a.hasTarget && !a.finished; // on its way again
                        a.velocityDirty = a.steering;               // or moving as it was
                    });
            }
        }

        // A warp: the agent out of its crowd and into the crowd of the zone holding the point (its
        // own or another), at the navmesh nearest it; the entity moved there too (the same
        // unparented-agent assumption as the writeback). Outside every zone it stays out of every
        // crowd, said once, as an agent placed there at the start is.
        void Warp(NavAgentComponent& a, scene::EntityHandle entity)
        {
            a.warpRequested = false;
            if (a.zoneIndex >= 0 && a.agentId >= 0)
            {
                m_zones[static_cast<usize>(a.zoneIndex)].crowd->RemoveAgent(a.agentId);
            }
            a.agentId = -1;
            a.zoneIndex = FindZoneContaining(a.warpTo);
            Transform t = m_scene->GetLocalTransform(entity);
            t.position = a.warpTo;
            m_scene->SetLocalTransform(entity, t);
            if (a.zoneIndex < 0)
            {
                LOG_WARNING(u8"Navigation", u8"agent '{}' warped to ({}, {}, {}), in no navigation zone; it will not move",
                            m_scene->GetEntityName(entity), a.warpTo.x, a.warpTo.y, a.warpTo.z);
                return;
            }
            RuntimeZone& rz = m_zones[static_cast<usize>(a.zoneIndex)];
            a.agentId = AddToCrowd(rz, a, a.warpTo);
            if (a.agentId >= 0)
            {
                // Where the crowd placed it: the navmesh under the point, not the point itself.
                t.position = TransformPoint(rz.crowd->AgentPosition(a.agentId), rz.world);
                m_scene->SetLocalTransform(entity, t);
            }
        }

        // An agent into a zone's crowd at `worldPos` with its own steering profile; its slot, or -1.
        i32 AddToCrowd(RuntimeZone& rz, NavAgentComponent& a, Float3 worldPos)
        {
            nav::NavigationAgentParams ap;
            ap.radius = a.radius;
            ap.height = a.height;
            ap.maxSpeed = a.maxSpeed;
            ap.maxAcceleration = a.maxAcceleration;
            ap.avoidOthers = a.avoidOthers;
            a.appliedSpeed = a.maxSpeed; // the live-change compare starts in sync
            a.appliedAcceleration = a.maxAcceleration;
            a.appliedAvoidOthers = a.avoidOthers;
            return rz.crowd->AddAgent(TransformPoint(worldPos, rz.invWorld), ap);
        }

        void RegisterAgents()
        {
            auto* agents = m_scene->GetSystem<NavAgentComponentManager>();
            if (agents == nullptr)
            {
                return;
            }
            agents->ForEach([this](NavAgentComponent& a, scene::EntityHandle entity) { Register(a, entity); });
        }

        // One agent into the crowd of the zone it stands in: every agent at the start, and one added
        // later (a spawned prefab's) on the first update that sees it. Once, whatever the outcome,
        // so an agent outside every zone is said once, not every frame.
        void Register(NavAgentComponent& a, scene::EntityHandle entity)
        {
            a.registered = true;
            const Float3 worldPos = m_scene->GetWorldPosition(entity);
            a.zoneIndex = FindZoneContaining(worldPos);
            a.agentId = -1;
            if (a.zoneIndex < 0)
            {
                // Said, not silent: an agent that never moves is otherwise a mystery.
                LOG_WARNING(u8"Navigation",
                            u8"agent '{}' at ({}, {}, {}) is in no navigation zone; it will not move",
                            m_scene->GetEntityName(entity), worldPos.x, worldPos.y, worldPos.z);
                return;
            }
            RuntimeZone& rz = m_zones[static_cast<usize>(a.zoneIndex)];
            a.agentId = AddToCrowd(rz, a, worldPos);
            if (a.agentId < 0)
            {
                LOG_WARNING(u8"Navigation",
                            u8"agent '{}' at ({}, {}, {}): the zone's crowd is full; it will not move",
                            m_scene->GetEntityName(entity), worldPos.x, worldPos.y, worldPos.z);
            }
            else if (rz.crowd->AgentState(a.agentId).state == nav::NavAgentCrowdState::Invalid)
            {
                // Detour still adds an agent it could not place, in a state that never
                // moves, so the slot alone says nothing.
                LOG_WARNING(u8"Navigation",
                            u8"agent '{}' at ({}, {}, {}) found no navmesh where it stands; it "
                            u8"will not move (is it on baked ground, and was the zone baked?)",
                            m_scene->GetEntityName(entity), worldPos.x, worldPos.y, worldPos.z);
            }
        }

        // First zone whose (world-space, axis-aligned from center and extents) box contains
        // `worldPos`.
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
        UniquePtr<TaggedAllocator> m_allocator;
        Array<RuntimeZone> m_zones;
        NavigationSceneSettings m_settings;
        BakeStageCache m_bakeStages;
    };

    // Per-scene manager set (the same call the composition and the runtime injection use).
    inline void AddNavigationSceneManagers(scene::Scene& scene)
    {
        scene.AddSystem<NavMeshZoneComponentManager>();
        scene.AddSystem<NavAgentComponentManager>();
        scene.AddSystem<NavigationSceneSystem>();
    }

    // Runtime subsystem: the app-registered broker that injects the per-scene navigation managers
    // into every scene (through the composition) and registers the component reflection once. The
    // per-scene tick lives in NavigationSceneSystem; this type owns no cross-scene state.
    class NavigationSubsystem final : public foundation::runtime::Subsystem,
                                      public scene::ISceneObserver
    {
    public:
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

        // Persistent debug draw, as physics does: when a scene's NavigationSceneSettings.debugDraw
        // is on, draw its loaded navmesh (and agent paths) into the render debug scene each frame.
        // Defined in the implementation unit, so the render dependency stays out of this interface.
        void Update(f32 deltaTime) override;

    protected:
        void OnInit() override { RegisterNavigationComponentReflection(); }
        void OnReady() override
        {
            if (foundation::runtime::SubsystemManager* context = Manager())
            {
            if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
            {
                scenes->RegisterObserver(this, scene::SceneLifecycleStage::SystemsReady);
                scenes->RegisterObserver(this, scene::SceneLifecycleStage::Destroying);
            }
            }
        }
        void OnShutdown() override
        {
            if (foundation::runtime::SubsystemManager* context = Manager())
            {
            if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
            {
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

export namespace engine::navigation
{
    /// This domain's declaration: what it brings to a scene, to reflection and to the script
    /// surface, and which resource modules come with it. Defined in the implementation unit (one
    /// instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& NavigationDomain() noexcept;
}
