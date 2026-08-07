// Draconic::Physics - :world partition.
//
// PhysicsWorld: the Jolt-backed rigid-body world (docs/design/physics.md). Jolt is the
// COMMITTED backend - no abstraction layer - but JPH types never appear here AT ALL:
// this interface unit is Jolt-free (all Jolt contact lives in WorldImpl.cpp, a module
// IMPLEMENTATION unit) both for API hygiene and because GCC's C++20-modules serializer
// chokes on Jolt's header mass inside an interface unit's global fragment. Bodies are
// BodyId handles; shapes/queries/events speak engine types; the per-body u64 user data
// carries the scene-entity reverse map. One world per SCENE; stepping is driven from the
// engine's fixed-update lane.
//
// Layers (§3.3): a FIXED semantic table - Static / Dynamic / Kinematic / Trigger - with
// a hard-coded collision matrix (ez-style named matrix; triggers are Jolt sensors:
// overlap events, no response).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.physics:world;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::physics
{
    // ---- public vocabulary (engine types only) ----

    enum class PhysicsLayer : u8
    {
        Static = 0, // immovable level geometry
        Dynamic,    // simulated bodies
        Kinematic,  // scene-driven movers (platforms)
        Trigger,    // sensors: overlap events, no collision response
        Count,
    };

    enum class MotionKind : u8
    {
        Static,
        Kinematic,
        Dynamic
    };

    enum class ShapeKind : u8
    {
        Box,
        Sphere,
        Capsule,
        Cooked,
        Plane
    };

    // One shape (a body carries one or more; >1 = compound).
    struct ShapeDesc
    {
        ShapeKind kind = ShapeKind::Box;
        Float3 halfExtents{0.5f, 0.5f, 0.5f};   // Box
        f32 radius = 0.5f;                      // Sphere / Capsule
        f32 halfHeight = 0.5f;                  // Capsule (cylinder half-length)
        Float3 localPosition{0.0f, 0.0f, 0.0f}; // compound child placement
        Quaternion localRotation = Quaternion::Identity;
        /// ShapeKind::Cooked: a blob from CookConvexHull/CookTriangleMesh (self-describing;
        /// convex hulls may be dynamic, triangle meshes MUST be static/kinematic). The span
        /// is only read during CreateBody - the caller keeps ownership (resource memory).
        Span<const byte> cooked;
        /// Non-uniform shape scale (cooked level/prop geometry authored at unit scale);
        /// {1,1,1} = none. Triangle meshes accept any scale, convex hulls uniform-ish only.
        Float3 scale{1.0f, 1.0f, 1.0f};
        /// ShapeKind::Plane: dot(planeNormal, p) + planeDistance = 0, the NEGATIVE half
        /// space solid; infinite in principle but only collidable within +-planeHalfExtent
        /// of the shape origin (keep as tight as the scene allows - broad-phase cost).
        /// Planes must be static/kinematic, never dynamic.
        Float3 planeNormal{0.0f, 1.0f, 0.0f};
        f32 planeDistance = 0.0f;
        f32 planeHalfExtent = 1000.0f;
    };

    struct BodyDesc
    {
        MotionKind motion = MotionKind::Dynamic;
        PhysicsLayer layer = PhysicsLayer::Dynamic;
        Array<ShapeDesc> shapes; // >= 1; several = compound
        Float3 position{0.0f, 0.0f, 0.0f};
        Quaternion rotation = Quaternion::Identity;
        f32 density = 1000.0f; // kg/m^3 (Jolt convention)
        f32 friction = 0.5f;
        f32 restitution = 0.0f;
        f32 linearDamping = 0.05f;
        f32 angularDamping = 0.05f;
        bool isTrigger = false; // sensor (forces layer Trigger)
        /// Designer collision group [0, 32): pairs collide only when the world matrix
        /// allows BOTH directions' bits (PhysicsWorldSettings::groupCollides). The
        /// semantic layer rules (static-static never, trigger sensing) still apply.
        u8 group = 0;
        u64 userData = 0; // scene-entity reverse map (guid low bits)
    };

    inline constexpr u32 kCollisionGroupCount = 32;

    struct BodyId
    {
        u32 value = 0xFFFFFFFFu;
        [[nodiscard]] bool IsValid() const noexcept { return value != 0xFFFFFFFFu; }
        friend bool operator==(BodyId a, BodyId b) noexcept { return a.value == b.value; }
    };

    struct RayHit
    {
        BodyId body;
        u64 userData = 0;
        Float3 position{0, 0, 0};
        Float3 normal{0, 0, 0};
        f32 fraction = 1.0f;
        /// Material-slot index of the hit face for cooked TRIANGLE-MESH shapes (whatever
        /// the cooker stored per triangle - the source mesh's material slot); 0 otherwise.
        u32 surface = 0;
    };

    enum class ContactKind : u8
    {
        Begin,
        End,
        TriggerEnter,
        TriggerExit
    };

    struct ContactEvent
    {
        ContactKind kind = ContactKind::Begin;
        BodyId bodyA;
        BodyId bodyB;
        u64 userA = 0;
        u64 userB = 0;
        /// Contact geometry (Begin/TriggerEnter only; zero for End/TriggerExit which have
        /// no manifold). `point`/`normal` are world-space (normal points from body B toward
        /// body A). `speed` is the impact APPROACH SPEED: the magnitude of the two bodies'
        /// relative velocity projected onto the contact normal - "how hard did they hit".
        /// It is deliberately NOT a solver impulse: Jolt does not surface the true impulse
        /// cleanly in OnContactAdded, so approach speed is the honest, physically-meaningful
        /// value (see WorldImpl.cpp OnContactAdded).
        Float3 point{0, 0, 0};
        Float3 normal{0, 0, 0};
        f32 speed = 0.0f;
    };

    // ---- joints (P3) ----

    enum class JointKind : u8
    {
        Fixed,
        Point,
        Hinge,
        Slider,
        Distance
    };

    struct JointDesc
    {
        JointKind kind = JointKind::Fixed;
        BodyId bodyA;                    // required
        BodyId bodyB;                    // invalid = anchored to the WORLD
        Float3 anchor{0.0f, 0.0f, 0.0f}; // world-space pivot (hinge/point/slider origin)
        Float3 axis{0.0f, 1.0f, 0.0f};   // world-space hinge rotation / slider travel axis
        /// Hinge: radians (min in [-pi,0], max in [0,pi]); Slider: meters around the rest
        /// point. min > max = unlimited.
        f32 limitMin = 1.0f;
        f32 limitMax = -1.0f;
        /// Distance joints: negative = keep the starting distance.
        f32 minDistance = -1.0f;
        f32 maxDistance = -1.0f;
        /// Velocity motor (hinge: rad/s + torque limit; slider: m/s + force limit).
        bool motorEnabled = false;
        f32 motorTargetVelocity = 0.0f;
        f32 motorLimit = 3.4e38f;
    };

    struct JointId
    {
        u32 value = 0xFFFFFFFFu;
        [[nodiscard]] bool IsValid() const noexcept { return value != 0xFFFFFFFFu; }
        friend bool operator==(JointId a, JointId b) noexcept { return a.value == b.value; }
    };

    // ---- character controller (P3): Jolt CharacterVirtual ----
    // Not a rigid body: a kinematic capsule swept by UpdateCharacter with slope/step
    // handling; pushes dynamic bodies up to maxStrength. The caller owns the velocity
    // policy (gravity/jump folded into SetCharacterVelocity each step - the component
    // layer implements the standard recipe).

    struct CharacterDesc
    {
        f32 capsuleRadius = 0.35f;
        f32 capsuleHalfHeight = 0.55f; // cylinder half-length (total height = 2*(hh+r))
        f32 maxSlopeDegrees = 50.0f;
        f32 mass = 70.0f;         // kg (impulses given to pushed bodies)
        f32 maxStrength = 100.0f; // max push force (N)
        f32 stepUp = 0.4f;        // stair climb per step
        f32 stepDown = 0.5f;      // stick-to-floor scan below
        Float3 position{0.0f, 0.0f, 0.0f};
        u64 userData = 0;
    };

    struct CharacterId
    {
        u32 value = 0xFFFFFFFFu;
        [[nodiscard]] bool IsValid() const noexcept { return value != 0xFFFFFFFFu; }
        friend bool operator==(CharacterId a, CharacterId b) noexcept { return a.value == b.value; }
    };

    enum class CharacterGround : u8
    {
        OnGround,
        OnSteepGround,
        NotSupported,
        InAir
    };

    struct PhysicsWorldSettings
    {
        Float3 gravity{0.0f, -9.81f, 0.0f};
        u32 maxBodies = 4096;
        u32 maxBodyPairs = 4096;
        u32 maxContactConstraints = 2048;
        /// Group matrix: bit j of entry i = "group i collides with group j" (kept
        /// symmetric by writers; the filter tests i->j only). Default: everything
        /// collides with everything.
        u32 groupCollides[kCollisionGroupCount] = {
            0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
            0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
            0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
            0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
            0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
            0xFFFFFFFFu, 0xFFFFFFFFu};
    };

    // ---- offline shape cooking (builder/editor side; blobs feed ShapeKind::Cooked) ----
    // The blob format is Jolt's binary shape state: self-describing (convex vs mesh),
    // versioned by Jolt - cooked products must recook on a Jolt upgrade, which the
    // asset pipeline's builder-version bump handles.

    /// Convex hull from a point cloud. `hullTolerance` trades vertex count for fidelity
    /// (points may sit this far outside the hull; larger = simpler hull).
    /// False on degenerate input (< 4 non-coplanar points).
    [[nodiscard]] bool CookConvexHull(Span<const Float3> points, Array<byte>& outBlob,
                                      f32 hullTolerance = 1.0e-3f);

    /// Static triangle mesh. `triangleMaterialSlots` (optional: empty = all 0) carries one
    /// material-slot index per triangle, surfaced on ray hits as RayHit::surface.
    /// `indices` size must be a multiple of 3. False on empty/malformed input.
    [[nodiscard]] bool CookTriangleMesh(Span<const Float3> positions, Span<const u32> indices,
                                        Span<const u32> triangleMaterialSlots,
                                        Array<byte>& outBlob);

    /// Triangles of a cooked blob (debug/gizmo outline geometry: 3 positions per triangle).
    /// False if the blob doesn't restore.
    [[nodiscard]] bool ExtractShapeTriangles(Span<const byte> blob, Array<Float3>& outTriangles);

    class PhysicsWorld
    {
    public:
        explicit PhysicsWorld(const PhysicsWorldSettings& settings = {});
        ~PhysicsWorld();
        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        void SetGravity(Float3 gravity);
        [[nodiscard]] Float3 Gravity() const;

        // ---- bodies ----
        [[nodiscard]] BodyId CreateBody(const BodyDesc& desc);
        void DestroyBody(BodyId id);
        [[nodiscard]] usize BodyCount() const;

        // ---- stepping ----
        /// One fixed step. Contact events buffered during the step are available from
        /// DrainContacts() afterwards.
        void Step(f32 deltaTime, i32 collisionSteps = 1);

        // ---- transforms & motion ----
        void GetBodyTransform(BodyId id, Float3& outPosition, Quaternion& outRotation) const;
        /// Teleport: snaps the body (velocities untouched). Dynamic bodies mid-sim should
        /// use this, never per-frame scene writes (transform ownership, §3.2).
        void SetBodyTransform(BodyId id, Float3 position, Quaternion rotation);
        /// Velocity-correct kinematic move over `deltaTime` (the fixed step).
        void MoveKinematic(BodyId id, Float3 position, Quaternion rotation, f32 deltaTime);
        void SetLinearVelocity(BodyId id, Float3 velocity);
        [[nodiscard]] Float3 LinearVelocity(BodyId id) const;
        void AddImpulse(BodyId id, Float3 impulse);
        void AddForce(BodyId id, Float3 force);
        [[nodiscard]] bool IsActive(BodyId id) const;
        [[nodiscard]] u64 UserData(BodyId id) const;

        // ---- queries ----
        /// `groupMask`: bit g = consider bodies in group g (default: all groups).
        [[nodiscard]] bool RayCast(Float3 from, Float3 direction, f32 maxDistance, RayHit& out,
                                   u32 groupMask = 0xFFFFFFFFu) const;
        /// Bodies whose shapes contain `point` (triggers included).
        void QueryPoint(Float3 point, Array<BodyId>& out, u32 groupMask = 0xFFFFFFFFu) const;

        // ---- joints ----
        [[nodiscard]] JointId CreateJoint(const JointDesc& desc);
        void DestroyJoint(JointId id);
        /// Velocity-motor control on hinge/slider joints (no-op on other kinds). The
        /// target is rad/s (hinge) or m/s (slider); `enabled` false turns the motor off.
        void SetJointMotor(JointId id, bool enabled, f32 targetVelocity);

        // ---- character controllers ----
        [[nodiscard]] CharacterId CreateCharacter(const CharacterDesc& desc);
        void DestroyCharacter(CharacterId id);
        /// The FULL velocity for the coming update (the caller folds gravity/jump in).
        void SetCharacterVelocity(CharacterId id, Float3 velocity);
        [[nodiscard]] Float3 CharacterVelocity(CharacterId id) const;
        /// Sweeps the character (slide + stairs + stick-to-floor) against the world.
        /// Call once per fixed step, after Step().
        void UpdateCharacter(CharacterId id, f32 deltaTime);
        [[nodiscard]] Float3 CharacterPosition(CharacterId id) const;
        void SetCharacterPosition(CharacterId id, Float3 position); // teleport
        [[nodiscard]] CharacterGround GetCharacterGround(CharacterId id) const;

        // ---- contact events ----
        /// Moves the events buffered since the last drain (worker-thread listeners append
        /// under a mutex; the fixed-step driver drains on the main thread).
        void DrainContacts(Array<ContactEvent>& out);

    private:
        struct Impl;
        UniquePtr<Impl> m_impl;
    };
}
