// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Navigation - :mesh partition.
//
// The Detour runtime wrappers: NavigationMesh (a loaded tiled navmesh), its query
// (NavigationMeshQuery, for pathfinding) and its crowd (NavigationCrowd, agents that steer to
// targets and avoid each other). Each is PIMPL: the Detour dt* types live only in
// NavigationImpl.cpp. A query or crowd borrows its mesh's Detour handle, so the NavigationMesh
// has to outlive every query and crowd built against it (the engine subsystem owns all three
// per zone, so this holds).

module;
#include "Core/Prelude.h"

export module foundation.navigation:mesh;

import foundation.core;

using namespace foundation::core;

export namespace foundation::navigation
{
    // A straight-line path across the navmesh (corner points from start to end). `complete`
    // is false when the destination could not be reached; `corners` then leads to the nearest
    // reachable point. An empty `corners` with a failing Status means start/end were off-mesh.
    struct NavigationPath
    {
        Array<Float3> corners;
        bool complete = false;
    };

    // A live crowd agent's introspection snapshot (AgentState). The values are the engine's own
    // stable contract, remapped from Detour's internals, so scripts and tools can key on them.
    enum class NavAgentCrowdState : u8
    {
        Invalid = 0, // not a live agent
        Walking = 1, // steering on the mesh
        OffMesh = 2, // traversing an off-mesh connection
    };
    enum class NavAgentTargetState : u8
    {
        None = 0,       // no move request
        Requesting = 1, // pathfind queued or running (any Detour in-flight stage)
        Valid = 2,      // moving on a valid corridor
        Velocity = 3,   // velocity-driven (no corridor)
        Failed = 4,     // pathfind failed
    };
    struct NavigationAgentState
    {
        bool valid = false;
        NavAgentCrowdState state = NavAgentCrowdState::Invalid;
        NavAgentTargetState targetState = NavAgentTargetState::None;
        f32 desiredSpeed = 0.0f; // the crowd's current speed intent (<= maxSpeed)
        i32 cornerCount = 0;     // corridor corners still ahead (path progress hint)
    };

    // Per-agent steering profile for the crowd.
    struct NavigationAgentParams
    {
        f32 radius = 0.6f;
        f32 height = 2.0f;
        f32 maxSpeed = 3.5f;
        f32 maxAcceleration = 8.0f;
        // Steer round the others (velocity-sampled avoidance and separation). Off, the agent goes
        // straight where it is sent (a player's character) and shoulders past anyone in its way:
        // the crowd's collision step pushes the two apart, softly.
        bool avoidOthers = true;
    };

    // A loaded tiled navmesh (from NavigationMeshBuilder::Build output). Move-only; owns
    // the Detour navmesh. The allocator (required; the owner decides) backs the impl block.
    class NavigationMesh
    {
    public:
        explicit NavigationMesh(IAllocator& allocator);
        ~NavigationMesh();
        NavigationMesh(NavigationMesh&&) noexcept;
        NavigationMesh& operator=(NavigationMesh&&) noexcept;
        NavigationMesh(const NavigationMesh&) = delete;
        NavigationMesh& operator=(const NavigationMesh&) = delete;

        // Load a serialized navmesh blob (Build output). Replaces any prior contents.
        [[nodiscard]] Status Load(Span<const byte> data);
        [[nodiscard]] bool IsValid() const noexcept;

        // The agent radius/height the mesh was baked for (recovered from the blob header). Zero
        // when invalid. The crowd uses the radius to size local avoidance.
        [[nodiscard]] f32 BakedAgentRadius() const noexcept;
        [[nodiscard]] f32 BakedAgentHeight() const noexcept;

        // Partial rebake: swap one tile of a loaded tiled navmesh (remove, then add) with fresh
        // Detour tile data (BuildTileInGrid output). Empty data removes the tile. Fails for
        // a single-tile grid (it cannot place arbitrary tiles) and for invalid data.
        // Queries and crowds over this mesh see the change immediately.
        [[nodiscard]] Status ReplaceTile(i32 tileX, i32 tileY, Span<const byte> tileData);

        // Append the navmesh's walkable triangles (in navmesh-local space, 3 verts per triangle)
        // to `out` for debug draw. Drawn from the live dtNavMesh, so it reflects exactly what
        // queries path on (it catches load, version or transform drift a bake-time outline would
        // hide).
        // Does not clear `out` (append semantics). No-op when invalid.
        void DebugTriangles(Array<Float3>& out) const;

        // Internal: the Detour dtNavMesh* as an opaque handle, for query and crowd construction
        // inside foundation.navigation only. Null when invalid. Do not reinterpret elsewhere.
        [[nodiscard]] void* NativeHandle() const noexcept;

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };

    // Pathfinding against a NavigationMesh. Move-only. The mesh must outlive the query.
    class NavigationMeshQuery
    {
    public:
        NavigationMeshQuery(IAllocator& allocator, const NavigationMesh& mesh);
        ~NavigationMeshQuery();
        NavigationMeshQuery(NavigationMeshQuery&&) noexcept;
        NavigationMeshQuery& operator=(NavigationMeshQuery&&) noexcept;
        NavigationMeshQuery(const NavigationMeshQuery&) = delete;
        NavigationMeshQuery& operator=(const NavigationMeshQuery&) = delete;

        [[nodiscard]] bool IsValid() const noexcept;

        // Path from `start` to `end`. Ok + out.complete=true when reachable; Ok +
        // out.complete=false when the nearest reachable point falls short (unreachable
        // destination, reported, never a crash or a false success); a failing Status when
        // start or end has no polygon within the search box.
        [[nodiscard]] Status FindPath(Float3 start, Float3 end, NavigationPath& out) const;

        // Nearest point on the navmesh to `point` within the default search box. False if none.
        [[nodiscard]] bool FindNearestPoint(Float3 point, Float3& out) const;

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };

    // A Detour crowd bound to one NavigationMesh: agents steer toward per-agent targets and
    // avoid each other. Move-only. The mesh must outlive the crowd. One crowd per navmesh
    // (Detour builds its internal query against a single mesh).
    class NavigationCrowd
    {
    public:
        NavigationCrowd(IAllocator& allocator, const NavigationMesh& mesh, i32 maxAgents,
                        f32 maxAgentRadius);
        ~NavigationCrowd();
        NavigationCrowd(NavigationCrowd&&) noexcept;
        NavigationCrowd& operator=(NavigationCrowd&&) noexcept;
        NavigationCrowd(const NavigationCrowd&) = delete;
        NavigationCrowd& operator=(const NavigationCrowd&) = delete;

        [[nodiscard]] bool IsValid() const noexcept;

        // Add an agent at `position` (snapped to the nearest polygon). Returns an agent id
        // (>= 0) or -1 on failure (crowd full, off-mesh spawn, invalid crowd).
        [[nodiscard]] i32 AddAgent(Float3 position, const NavigationAgentParams& params);
        void RemoveAgent(i32 agentId);

        // Update a live agent's steering profile (speed, acceleration, radius, height): per-call
        // speed control without removing and re-adding it. No-op for invalid agents.
        void SetAgentParams(i32 agentId, const NavigationAgentParams& params);

        // Steer the agent toward `target` (snapped to the nearest polygon). False if the agent
        // is invalid or the target has no polygon within the search box.
        [[nodiscard]] bool SetTarget(i32 agentId, Float3 target);
        // Drive the agent at `velocity` (m/s, the crowd's space) instead of to a point: no path, the
        // crowd keeps it on the navmesh and avoiding others (a player's stick). False if invalid.
        [[nodiscard]] bool SetVelocity(i32 agentId, Float3 velocity);
        void ClearTarget(i32 agentId);

        void Update(f32 deltaTime);

        [[nodiscard]] Float3 AgentPosition(i32 agentId) const;
        [[nodiscard]] Float3 AgentVelocity(i32 agentId) const;
        [[nodiscard]] bool IsAgentValid(i32 agentId) const;

        // The live agent's internals (state machine, move-request stage, speed intent, path
        // progress), for debugging why an agent is or is not moving.
        [[nodiscard]] NavigationAgentState AgentState(i32 agentId) const;

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };
}
