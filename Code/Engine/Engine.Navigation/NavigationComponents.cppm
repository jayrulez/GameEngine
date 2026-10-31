// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Navigation - :components partition.
//
// The authoring components:
//   * NavMeshZoneComponent: a navmesh zone: AABB half-extents (the bake region), the bake
//     params (used by the editor's Bake action; the runtime ignores them), and a Ref to the
//     cooked NavigationZoneResource the subsystem loads.
//   * NavAgentComponent: an agent that steers to targets through the crowd. Reflected runtime
//     methods (navigate/stop/finished/remaining/velocity) write intent to transient fields; the
//     NavigationSceneSystem tick consumes them and (in MoveEntity mode) writes the transform.
//
// Runtime fields (agent id, zone index, target request, status) are transient and never serialized.

module;
#include "Core/Prelude.h"

export module engine.navigation:components;

import foundation.core;
import foundation.scene;
import foundation.resource;
import foundation.navigation.resource;

using namespace foundation::core;

export namespace engine::navigation
{
    namespace nav = foundation::navigation;

    struct NavMeshZoneComponent
    {
        // Authored: the zone AABB half-extents in the entity's local space (the bake region).
        Float3 extents{20.0f, 10.0f, 20.0f};
        // Authored bake profile (the editor Bake action reads these; the runtime does not).
        f32 cellSize = 0.3f;
        f32 cellHeight = 0.2f;
        f32 agentRadius = 0.6f;
        f32 agentHeight = 2.0f;
        f32 agentMaxClimb = 0.9f;
        f32 agentMaxSlopeDegrees = 45.0f;
        // The cooked navmesh this zone loads at runtime.
        foundation::resource::Ref<nav::NavigationZoneResource> zone;
        // The most agents its crowd moves at once (data version 2; a version 1 zone keeps the
        // default). A crowd reserves its agents up front, so ask for what the scene will use.
        i32 maxAgents = 128;

        // Runtime (transient): index into the subsystem's live-zone list, -1 until started.
        i32 runtimeIndex = -1;
    };

    struct NavAgentComponent
    {
        // Authored:
        f32 radius = 0.6f;
        f32 height = 2.0f;
        f32 maxSpeed = 3.5f;
        f32 maxAcceleration = 8.0f;
        // Arrival radius: the agent counts as finished (and stops steering) within this distance of
        // the target, for "walk near the door", following at a distance, or surrounding. 0 walks
        // onto the point itself.
        f32 stopDistance = 0.0f;
        // MoveEntity (default): the agent writes the entity transform from crowd output.
        // ReportOnly: the entity is not moved; script or physics reads the desired velocity
        // instead.
        bool moveEntity = true;
        // Steer round other agents (on, the default: a guest in a crowd). Off, it goes straight
        // where it is sent and shoulders past anyone in its way, the crowd pushing them apart (a
        // player's character: avoiding, a velocity-driven agent holds back from anyone ahead of it).
        bool avoidOthers = true;

        // Runtime (transient):
        i32 agentId = -1;   // crowd agent handle within its zone, -1 = not registered
        bool registered = false; // tried for a crowd (at the start, or on the first update after it was added)
        i32 zoneIndex = -1; // subsystem live-zone index, -1 = outside every zone
        Float3 target{0, 0, 0};
        bool hasTarget = false;
        bool targetDirty = false;   // a navigate() request the tick has not applied yet
        bool stopRequested = false; // a stop() request the tick has not applied yet
        bool warpRequested = false; // a warp() request the tick has not applied yet
        bool steering = false;      // moving by move() (a velocity), not to a target
        bool velocityDirty = false; // a move() request the tick has not applied yet
        Float3 steerVelocity{0, 0, 0};
        Float3 warpTo{0, 0, 0};
        bool finished = true;
        f32 remainingDistance = 0.0f;
        Float3 desiredVelocity{0, 0, 0}; // crowd steering output (world space), for ReportOnly
        f32 appliedSpeed = -1.0f;        // steering profile last pushed into the crowd; the
        f32 appliedAcceleration = -1.0f; // tick re-applies on any change (script or inspector)
        bool appliedAvoidOthers = true;
        // Introspection cache (filled from the crowd each tick; see the foundation enums):
        u8 crowdState = 0;       // NavAgentCrowdState: 0 invalid, 1 walking, 2 off-mesh
        u8 crowdTargetState = 0; // NavAgentTargetState: 0 none, 1 requesting, 2 valid,
                                 // 3 velocity, 4 failed
        f32 crowdDesiredSpeed = 0.0f; // the crowd's current speed intent
        i32 pathCorners = 0;          // corridor corners ahead (path progress hint)

        // --- reflected runtime API (script: NavAgent.of(entity).navigate(x,y,z) ...) ---
        // Steer toward a world-space destination.
        void navigate(f32 x, f32 y, f32 z)
        {
            target = Float3{x, y, z};
            steering = false;
            hasTarget = true;
            targetDirty = true;
            stopRequested = false;
            finished = false;
        }
        // Halt where the agent is.
        void stop()
        {
            steering = false;
            hasTarget = false;
            stopRequested = true;
            finished = true;
        }
        // Put the agent somewhere else at once (a respawn, a door to another room, a late joiner
        // placed): the crowd owns an agent's position, so moving its entity is undone next tick.
        // Applied next tick: out of its crowd and back in at the nearest navmesh to the point (in
        // whichever zone holds it), stopped, the entity there too.
        void warp(f32 x, f32 y, f32 z)
        {
            warpTo = Float3{x, y, z};
            warpRequested = true;
            steering = false;
            hasTarget = false;
            targetDirty = false;
            stopRequested = false;
            finished = true;
        }
        // Move at a velocity across the ground (m/s, world X and Z) instead of to a point: a
        // player's stick, sent each frame. No path is planned, so it is answered at once; the crowd
        // keeps the agent on the navmesh and out of others' way, and others out of its. Lasts until
        // navigate(), stop() or warp(); move(0, 0) stands still, still in the crowd.
        void move(f32 vx, f32 vz)
        {
            steerVelocity = Float3{vx, 0, vz};
            steering = true;
            velocityDirty = true;
            hasTarget = false;
            targetDirty = false;
            stopRequested = false;
            finished = vx == 0.0f && vz == 0.0f;
        }
        // Per-call speed control: applies to the live crowd agent next tick.
        void setSpeed(f32 speed) { maxSpeed = speed; }
        void setStopDistance(f32 distance) { stopDistance = distance; }
        [[nodiscard]] f32 speed() const { return maxSpeed; }
        // navigate, speed and arrival radius in one call (the common scripted move order).
        void navigateAt(f32 x, f32 y, f32 z, f32 moveSpeed, f32 arriveDistance)
        {
            maxSpeed = moveSpeed;
            stopDistance = arriveDistance;
            navigate(x, y, z);
        }
        // Introspection: why an agent is or is not moving. Numeric contracts (the enums above),
        // stable for script logic and logging.
        [[nodiscard]] i32 state() const { return static_cast<i32>(crowdState); }
        [[nodiscard]] i32 targetState() const { return static_cast<i32>(crowdTargetState); }
        [[nodiscard]] f32 desiredSpeed() const { return crowdDesiredSpeed; }
        [[nodiscard]] i32 corners() const { return pathCorners; }
        [[nodiscard]] bool finishedNav() const { return finished; }
        [[nodiscard]] f32 remaining() const { return remainingDistance; }
        [[nodiscard]] f32 velocityX() const { return desiredVelocity.x; }
        [[nodiscard]] f32 velocityY() const { return desiredVelocity.y; }
        [[nodiscard]] f32 velocityZ() const { return desiredVelocity.z; }
    };

    // Per-scene navigation settings (the NavigationSceneSystem's settings block). `debugDraw`
    // renders the loaded navmesh surface; `debugDrawPaths` adds agent target lines. Both work in
    // the editor and the player, as the physics debug draw does: gated here, drawn by the runtime
    // subsystem.
    struct NavigationSceneSettings
    {
        bool debugDraw = false;
        bool debugDrawPaths = false;
        // Bake-stage overlay: the last editor bake's intermediate Recast data (walkable spans and
        // region contours), showing how the bake arrived at the mesh. Cleared per bake; the live
        // dtNavMesh draw above stays the ground truth for queries.
        bool debugDrawBakeStages = false;
    };

    inline void SerializeNavigationSceneSettings(ISerializer& ar, NavigationSceneSettings& settings)
    {
        foundation::core::Serialize(ar, "debugDraw", settings.debugDraw);
        foundation::core::Serialize(ar, "debugDrawPaths", settings.debugDrawPaths);
        foundation::core::Serialize(ar, "debugDrawBakeStages", settings.debugDrawBakeStages);
    }

    inline void Serialize(ISerializer& ar, NavMeshZoneComponent& c)
    {
        foundation::core::Serialize(ar, "extents", c.extents);
        foundation::core::Serialize(ar, "cellSize", c.cellSize);
        foundation::core::Serialize(ar, "cellHeight", c.cellHeight);
        foundation::core::Serialize(ar, "agentRadius", c.agentRadius);
        foundation::core::Serialize(ar, "agentHeight", c.agentHeight);
        foundation::core::Serialize(ar, "agentMaxClimb", c.agentMaxClimb);
        foundation::core::Serialize(ar, "agentMaxSlopeDegrees", c.agentMaxSlopeDegrees);
        foundation::core::Serialize(ar, "zone", c.zone);
        if (ar.Mode() == SerializeMode::Write || ar.Version() >= 2) // a version 1 zone keeps the default
        {
            foundation::core::Serialize(ar, "maxAgents", c.maxAgents);
        }
    }

    inline void ResolveResources(foundation::resource::ResourceManager& manager,
                                 NavMeshZoneComponent& c)
    {
        c.zone.Bind(manager);
    }

    inline void Serialize(ISerializer& ar, NavAgentComponent& c)
    {
        foundation::core::Serialize(ar, "radius", c.radius);
        foundation::core::Serialize(ar, "height", c.height);
        foundation::core::Serialize(ar, "maxSpeed", c.maxSpeed);
        foundation::core::Serialize(ar, "maxAcceleration", c.maxAcceleration);
        foundation::core::Serialize(ar, "moveEntity", c.moveEntity);
        foundation::core::Serialize(ar, "stopDistance", c.stopDistance);
        if (ar.Mode() == SerializeMode::Write || ar.Version() >= 3) // a version 2 agent avoids others
        {
            foundation::core::Serialize(ar, "avoidOthers", c.avoidOthers);
        }
    }

    class NavMeshZoneComponentManager final
        : public foundation::scene::SerializableComponentManager<NavMeshZoneComponent>
    {
    public:
        NavMeshZoneComponentManager()
            : SerializableComponentManager<NavMeshZoneComponent>(u8"navigation.Zone")
        {
        }
    };

    class NavAgentComponentManager final
        : public foundation::scene::SerializableComponentManager<NavAgentComponent>
    {
    public:
        NavAgentComponentManager()
            : SerializableComponentManager<NavAgentComponent>(u8"navigation.Agent")
        {
        }

        // An agent whose component went (its entity destroyed, or the component removed), for the
        // navigation to take out of its crowd on its next update.
        struct ReleasedAgent
        {
            i32 zoneIndex = -1;
            i32 agentId = -1;
        };
        [[nodiscard]] Array<ReleasedAgent> TakeReleased()
        {
            Array<ReleasedAgent> out = Move(m_released);
            m_released.Clear();
            return out;
        }

    protected:
        void OnComponentDestroyed(NavAgentComponent& c, foundation::scene::EntityHandle) override
        {
            if (c.agentId >= 0)
            {
                m_released.PushBack(ReleasedAgent{c.zoneIndex, c.agentId});
            }
        }

    private:
        Array<ReleasedAgent> m_released;
    };

    // Registers the component reflection (displayName/category, the `.of` handle and agent methods,
    // the zone Ref picker). Defined in the implementation unit, since REFLECT_VALUE bodies stay out
    // of interface units for GCC's module handling.
    void RegisterNavigationComponentReflection();

    // Metadata-only script-facade registrar (NavigationDomain's registerScriptFacade): surfaces
    // NavAgentComponent to the script backends so behaviors can call
    // NavAgent.of(entity).navigate(...) and name the class in their prelude. Touches no device or
    // world.
    void RegisterNavigationScriptFacade();
}
