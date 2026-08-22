// Engine::Physics - the `engine.physics` module.
//
// Scene integration (docs/design/physics.md §3.2): a PhysicsSceneSystem per scene owns its
// PhysicsWorld; bodies build from RigidBodyComponents (+ descendant ColliderComponents
// compounding) at OnSceneStarted and tear down at OnSceneStopped. Per fixed step:
// kinematic bodies <- scene transforms (MoveKinematic, velocity-correct), the coalesced
// world step, contact drain, dynamic poses -> the component pose double-buffer. Every
// RENDER frame the subsystem writes scene transforms as lerp(prev, curr, FixedAlpha()) -
// the interpolation none of the surveyed engines had. Transform ownership: dynamic =
// physics owns pos/rot (scene edits ignored mid-sim); kinematic = scene owns; static =
// immutable while simulating.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h" // RTTI_OBJECT (the Physics facade)
#include "Profiler/Profiler.h"       // PROFILE_SCOPE (compiles to nothing when disabled)
#include <cmath>

export module engine.physics;

export import :components;

import foundation.core;
import foundation.profiler;
import foundation.runtime;
import foundation.scene;
import engine.scene;
import foundation.script;
import foundation.script.facades; // foundation::script::Entity (the raycast/contact hit entity)
import foundation.physics;
// NOTE: no render imports HERE - the debug-draw path lives in SubsystemImpl.cpp (a module
// implementation unit). Keeping heavyweight imports out of the interface matters for
// GCC's module loader (-fno-module-lazy consumers force-load the whole import graph).

using namespace foundation::core;
using namespace foundation::physics;
using namespace engine::scene;

export namespace engine::physics
{
    // Foundation aliases (sibling engine::* namespaces would otherwise shadow these).
    namespace scene = foundation::scene;

    // The body user word carries the owning entity handle. EntityHandle is {u32 index,
    // u32 generation} = exactly 64 bits and unique BY CONSTRUCTION (the generation rejects
    // a reused slot), so it packs losslessly - unlike the entity guid's low 64 bits, which
    // is a lossy projection of the 128-bit guid that two distinct entities can share. This
    // subsystem is the ONLY place that packs and unpacks the word (the World-level u64 stays
    // an opaque handle), so the helpers live here.
    [[nodiscard]] inline u64 PackEntity(scene::EntityHandle handle) noexcept
    {
        return (static_cast<u64>(handle.index) << 32) | static_cast<u64>(handle.generation);
    }
    [[nodiscard]] inline scene::EntityHandle UnpackEntity(u64 value) noexcept
    {
        return scene::EntityHandle{static_cast<u32>(value >> 32),
                                   static_cast<u32>(value & 0xFFFFFFFFu)};
    }

    /// A contact whose bodies have been resolved back to scene entities (invalid handles for
    /// a side whose body no longer maps to a live entity - e.g. an End event after a body was
    /// destroyed). Delivered by the physics subsystem to every registered IContactListener at
    /// the physics tick (a safe top level - never nested inside a script call).
    struct EntityContact
    {
        ContactKind kind = ContactKind::Begin;
        scene::Scene* scene = nullptr;
        scene::EntityHandle a;
        scene::EntityHandle b;
        Float3 point{0, 0, 0};
        Float3 normal{0, 0, 0};
        f32 speed = 0.0f;
    };

    /// A consumer of resolved contacts (the script subsystem implements this to route contacts
    /// into behavior handlers). Called at the physics tick, once per drained contact per
    /// registered listener; implementations MUST only enqueue (no re-entrant script calls).
    class IContactListener
    {
    public:
        virtual ~IContactListener() = default;
        virtual void OnContact(const EntityContact& contact) = 0;
    };

    class PhysicsSceneSystem final : public scene::SceneSystem
    {
    public:
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }

        // Scene-settings seam (edited in the scene inspector, persisted with the scene).
        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override
        {
            return &TypeOf<PhysicsSceneSettings>();
        }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &m_settings; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"physics"; }
        void SerializeSettings(ISerializer& ar) override
        {
            SerializePhysicsSceneSettings(ar, m_settings);
        }

        [[nodiscard]] PhysicsSceneSettings& Settings() noexcept { return m_settings; }
        [[nodiscard]] PhysicsWorld* World() noexcept { return m_world.Get(); }
        [[nodiscard]] Span<const ContactEvent> Events() const noexcept
        {
            return Span<const ContactEvent>{m_events.Data(), m_events.Size()};
        }

        // Last ray hit recorded by ScenePhysics.rayCast on THIS scene - the hit* accessors read it
        // (per-scene state, so no bound-context service is needed to answer the follow-up questions).
        [[nodiscard]] const RayHit& LastHit() const noexcept { return m_lastHit; }
        [[nodiscard]] bool LastHitValid() const noexcept { return m_lastHitValid; }
        void SetLastHit(const RayHit& hit, bool valid) noexcept
        {
            m_lastHit = hit;
            m_lastHitValid = valid;
        }

        /// The subsystem points this at its listener list (stable address); each drained
        /// contact batch is resolved to entities and pushed to every listener. Null = no
        /// dispatch (a bare scene-system harness with no subsystem driving it).
        void SetContactListeners(const Array<IContactListener*>* listeners) noexcept
        {
            m_listeners = listeners;
        }

        // ---- play lifecycle ----

        void OnSceneStarted() override
        {
            // Scene::Start guarantees world matrices are current before this fires - bodies
            // build from the authored layout (never from pre-first-update Identity matrices).
            PhysicsWorldSettings settings;
            settings.gravity = m_settings.gravity;
            for (usize i = 0; i < m_settings.groupCollides.Size() && i < kCollisionGroupCount; ++i)
            {
                settings.groupCollides[i] = m_settings.groupCollides[i];
            }
            m_world = MakeUnique<PhysicsWorld>(DefaultAllocator(), settings);
            BuildBodies();
            BuildJoints();
            BuildCharacters();
        }

        void OnSceneStopped() override
        {
            auto* bodies = m_scene->GetSystem<RigidBodyComponentManager>();
            if (bodies != nullptr)
            {
                bodies->ForEach(
                    [](RigidBodyComponent& c, scene::EntityHandle)
                    {
                        c.body = BodyId{};
                        c.simActive = false;
                    });
            }
            if (auto* joints = m_scene->GetSystem<JointComponentManager>())
            {
                joints->ForEach(
                    [](JointComponent& c, scene::EntityHandle)
                    {
                        c.joint = JointId{};
                        c.simActive = false;
                    });
            }
            if (auto* characters = m_scene->GetSystem<CharacterComponentManager>())
            {
                characters->ForEach(
                    [](CharacterComponent& c, scene::EntityHandle)
                    {
                        c.character = CharacterId{};
                        c.simActive = false;
                    });
            }
            m_events.Clear();
            m_world = nullptr;
        }

        void OnFixedUpdate(f32 fixedDeltaTime) override
        {
            if (m_world.Get() == nullptr)
            {
                return;
            }
            scene::Scene& scene = *m_scene;
            auto* bodies = scene.GetSystem<RigidBodyComponentManager>();
            if (bodies == nullptr)
            {
                return;
            }
            PROFILE_SCOPE("Physics.Step");

            ReconcileActiveState(); // active edges settle BEFORE this step simulates

            // Kinematics follow the SCENE (velocity-correct move toward this step's target).
            bodies->ForEach(
                [&](RigidBodyComponent& c, scene::EntityHandle e)
                {
                    if (!c.body.IsValid() || c.motion != MotionKind::Kinematic)
                    {
                        return;
                    }
                    Float3 position;
                    Quaternion rotation;
                    Float3 scale;
                    if (Decompose(scene.GetWorldMatrix(e), position, rotation, scale))
                    {
                        m_world->MoveKinematic(c.body, position, rotation, fixedDeltaTime);
                    }
                });

            // Motor sync: component fields are LIVE (inspector/gameplay edits apply next step).
            if (auto* joints = scene.GetSystem<JointComponentManager>())
            {
                joints->ForEach(
                    [&](JointComponent& c, scene::EntityHandle)
                    {
                        if (c.joint.IsValid())
                        {
                            m_world->SetJointMotor(c.joint, c.motorEnabled, c.motorTargetVelocity);
                        }
                    });
            }

            m_world->Step(fixedDeltaTime,
                          m_settings.collisionSteps < 1 ? 1 : m_settings.collisionSteps);

            m_events.Clear();
            m_world->DrainContacts(m_events);
            // PUSH each drained batch at the physics tick: m_events is cleared every substep,
            // so a frame with multiple substeps would lose all but the last if listeners only
            // pulled Events() once per frame. Dispatch here (a safe top level - not nested in
            // any script call); listeners only enqueue.
            DispatchContacts();

            // Characters: the standard velocity recipe (grounded = planar move + one-shot
            // jump; airborne = keep gravity-integrated fall, steer planar), then sweep.
            if (auto* characters = scene.GetSystem<CharacterComponentManager>())
            {
                const Float3 gravity = m_world->Gravity();
                characters->ForEach(
                    [&](CharacterComponent& c, scene::EntityHandle)
                    {
                        if (!c.character.IsValid())
                        {
                            return;
                        }
                        // Teleport/respawn (setPosition): snap the CharacterVirtual, drop momentum,
                        // and skip this step's integration so the snap is exact (interpolation snaps
                        // too - prev == curr). Ground re-evaluates on the next step.
                        if (c.teleportPending)
                        {
                            m_world->SetCharacterPosition(c.character, c.teleportTo);
                            m_world->SetCharacterVelocity(c.character, Float3{0, 0, 0});
                            c.moveVelocity = Float3{0, 0, 0};
                            c.jumpSpeed = 0.0f;
                            c.prevPosition = c.teleportTo;
                            c.currPosition = c.teleportTo;
                            c.ground = CharacterGround::InAir;
                            c.teleportPending = false;
                            return;
                        }
                        const Float3 current = m_world->CharacterVelocity(c.character);
                        Float3 velocity{c.moveVelocity.x, 0.0f, c.moveVelocity.z};
                        if (c.ground == CharacterGround::OnGround)
                        {
                            if (c.jumpSpeed > 0.0f)
                            {
                                velocity.y = c.jumpSpeed;
                                c.jumpSpeed = 0.0f;
                            }
                        }
                        else
                        {
                            velocity.y = current.y + gravity.y * fixedDeltaTime;
                        }
                        m_world->SetCharacterStrength(c.character, c.maxStrength); // live push force
                        m_world->SetCharacterVelocity(c.character, velocity);
                        m_world->UpdateCharacter(c.character, fixedDeltaTime);
                        c.ground = m_world->GetCharacterGround(c.character);
                        c.prevPosition = c.currPosition;
                        c.currPosition = m_world->CharacterPosition(c.character);
                    });
            }

            // Dynamic poses into the double-buffer (prev <- curr <- world).
            bodies->ForEach(
                [&](RigidBodyComponent& c, scene::EntityHandle)
                {
                    if (!c.body.IsValid() || c.motion != MotionKind::Dynamic)
                    {
                        return;
                    }
                    c.prevPosition = c.currPosition;
                    c.prevRotation = c.currRotation;
                    m_world->GetBodyTransform(c.body, c.currPosition, c.currRotation);
                });
        }

        /// Render-frame interpolation (driven by the subsystem with the engine's fixed
        /// alpha): dynamic entities' scene transforms = lerp(prev, curr, alpha). Writes
        /// WORLD poses converted to local against the current parent.
        void ApplyInterpolation(f32 alpha)
        {
            if (m_world.Get() == nullptr || m_scene == nullptr || !m_scene->SimulationEnabled())
            {
                return;
            }
            scene::Scene& scene = *m_scene;
            auto* bodies = scene.GetSystem<RigidBodyComponentManager>();
            if (bodies == nullptr)
            {
                return;
            }
            bodies->ForEach(
                [&](RigidBodyComponent& c, scene::EntityHandle e)
                {
                    if (!c.body.IsValid() || c.motion != MotionKind::Dynamic)
                    {
                        return;
                    }
                    const Float3 position{
                        c.prevPosition.x + (c.currPosition.x - c.prevPosition.x) * alpha,
                        c.prevPosition.y + (c.currPosition.y - c.prevPosition.y) * alpha,
                        c.prevPosition.z + (c.currPosition.z - c.prevPosition.z) * alpha};
                    const Quaternion rotation = Slerp(c.prevRotation, c.currRotation, alpha);

                    // World -> local against the parent (scale preserved from the current local).
                    Transform local = scene.GetLocalTransform(e);
                    scene::EntityHandle parent = scene.GetParent(e);
                    if (parent.IsAssigned())
                    {
                        const Float4x4 world =
                            Transform{position, rotation, Float3{1, 1, 1}}.ToMatrix();
                        const Float4x4 parentInverse = Inverse(scene.GetWorldMatrix(parent));
                        Float3 lp, ls;
                        Quaternion lr;
                        if (Decompose(world * parentInverse, lp, lr, ls))
                        {
                            local.position = lp;
                            local.rotation = lr;
                        }
                    }
                    else
                    {
                        local.position = position;
                        local.rotation = rotation;
                    }
                    scene.SetLocalTransform(e, local);
                });

            if (auto* characters = scene.GetSystem<CharacterComponentManager>())
            {
                characters->ForEach(
                    [&](CharacterComponent& c, scene::EntityHandle e)
                    {
                        if (!c.character.IsValid())
                        {
                            return;
                        }
                        const Float3 position{
                            c.prevPosition.x + (c.currPosition.x - c.prevPosition.x) * alpha,
                            c.prevPosition.y + (c.currPosition.y - c.prevPosition.y) * alpha,
                            c.prevPosition.z + (c.currPosition.z - c.prevPosition.z) * alpha};
                        Transform local = scene.GetLocalTransform(e);
                        scene::EntityHandle parent = scene.GetParent(e);
                        if (parent.IsAssigned())
                        {
                            const Float4x4 world =
                                Transform{position, local.rotation, Float3{1, 1, 1}}.ToMatrix();
                            Float3 lp, ls;
                            Quaternion lr;
                            if (Decompose(world * Inverse(scene.GetWorldMatrix(parent)), lp, lr,
                                          ls))
                            {
                                local.position = lp;
                            }
                        }
                        else
                        {
                            local.position = position;
                        }
                        scene.SetLocalTransform(e, local); // rotation untouched (scene-owned)
                    });
            }
        }

        [[nodiscard]] scene::Scene* ScenePtr() const noexcept { return m_scene; }

    private:
        // After BuildBodies: joints reference the already-created bodies.
        void BuildJoints()
        {
            scene::Scene& scene = *m_scene;
            auto* joints = scene.GetSystem<JointComponentManager>();
            auto* bodies = scene.GetSystem<RigidBodyComponentManager>();
            if (joints == nullptr || bodies == nullptr)
            {
                return;
            }
            joints->ForEach(
                [&](JointComponent& c, scene::EntityHandle e)
                {
                    if (!scene.IsEffectivelyActive(e))
                    {
                        c.simActive = false; // reconciled on the activation edge
                        return;
                    }
                    c.simActive = true;
                    CreateJointForEntity(c, e, /*logFailures=*/true);
                });
        }

        // One entity's joint build (scene start + reconcile). `logFailures` only on the
        // scene-start path: the reconcile retries silently (a missing/invalid endpoint there
        // usually means "target currently inactive", which is a state, not a mistake). A
        // joint re-created on reactivation anchors at the entity's CURRENT pose (v1).
        void CreateJointForEntity(JointComponent& c, scene::EntityHandle e, bool logFailures)
        {
            scene::Scene& scene = *m_scene;
            auto* bodies = scene.GetSystem<RigidBodyComponentManager>();
            if (bodies == nullptr)
            {
                return;
            }
            {
                {
                    RigidBodyComponent* own = bodies->Get(e);
                    if (own == nullptr)
                    {
                        if (logFailures)
                        {
                            LOG_WARNING(u8"Physics",
                                                 u8"'{}': joint needs a rigid body on its entity",
                                                 scene.GetEntityName(e));
                        }
                        return;
                    }
                    if (!own->body.IsValid())
                    {
                        return; // body pending (inactive at start / failed) - reconcile retries
                    }
                    BodyId target; // invalid = world attachment
                    if (!c.targetEntity.IsNil())
                    {
                        scene::EntityHandle t = scene.FindEntity(c.targetEntity.id);
                        RigidBodyComponent* targetBody = t.IsAssigned() ? bodies->Get(t) : nullptr;
                        if (targetBody == nullptr)
                        {
                            if (logFailures)
                            {
                                LOG_WARNING(u8"Physics",
                                                     u8"'{}': joint target entity has no rigid "
                                                     u8"body - joint skipped",
                                                     scene.GetEntityName(e));
                            }
                            return;
                        }
                        if (!targetBody->body.IsValid())
                        {
                            return; // target inactive right now - rebuilt when it returns
                        }
                        target = targetBody->body;
                    }
                    else
                    {
                        for (scene::EntityHandle p = scene.GetParent(e); p.IsAssigned();
                             p = scene.GetParent(p))
                        {
                            RigidBodyComponent* parentBody = bodies->Get(p);
                            if (parentBody != nullptr && parentBody->body.IsValid())
                            {
                                target = parentBody->body;
                                break;
                            }
                        }
                    }

                    JointDesc desc;
                    desc.kind = c.kind;
                    desc.bodyA = own->body;
                    desc.bodyB = target;
                    const Float4x4 world = scene.GetWorldMatrix(e);
                    desc.anchor = TransformPoint(c.localAnchor, world);
                    Float3 position, scale;
                    Quaternion rotation;
                    desc.axis = Decompose(world, position, rotation, scale)
                                    ? RotateVector(rotation, c.localAxis)
                                    : c.localAxis;
                    desc.limitMin = c.limitMin;
                    desc.limitMax = c.limitMax;
                    desc.minDistance = c.minDistance;
                    desc.maxDistance = c.maxDistance;
                    desc.motorEnabled = c.motorEnabled;
                    desc.motorTargetVelocity = c.motorTargetVelocity;
                    desc.motorLimit = c.motorLimit;
                    c.joint = m_world->CreateJoint(desc);
                    if (!c.joint.IsValid() && logFailures)
                    {
                        LOG_WARNING(u8"Physics", u8"'{}': joint creation failed",
                                             scene.GetEntityName(e));
                    }
                }
            }
        }

        // Explicit-target readiness: nil target = world/ancestor attachment (always ready);
        // otherwise the target entity must resolve, be EFFECTIVELY ACTIVE, and have a live
        // body. The effective-active term matters because joints reconcile BEFORE bodies:
        // on the tick a target deactivates its body is still alive during the joint pass -
        // gating on body validity alone would keep the joint one tick past its dying body
        // (a Jolt constraint referencing a removed body). Activation is symmetric: the
        // rebuilt body appears a pass later, so the joint returns on the following tick
        // (the silent-retry path, as designed).
        [[nodiscard]] bool JointTargetReady(JointComponent& c)
        {
            if (c.targetEntity.IsNil())
            {
                return true;
            }
            scene::EntityHandle t = m_scene->FindEntity(c.targetEntity.id);
            if (!t.IsAssigned() || !m_scene->IsEffectivelyActive(t))
            {
                return false;
            }
            auto* bodies = m_scene->GetSystem<RigidBodyComponentManager>();
            RigidBodyComponent* tb = (bodies != nullptr) ? bodies->Get(t) : nullptr;
            return tb != nullptr && tb->body.IsValid();
        }

        // entity-active-state.md P3: reconcile the Jolt world against effective-active
        // EDGES (per-component latch - order-independent, self-healing, immune to the
        // load-before-components trap). Deactivation destroys the body/character/joint
        // (Jolt steps everything in its world - merely skipping the sync would NOT stop
        // it); activation rebuilds from the CURRENT pose with cleared momentum (v1).
        void ReconcileActiveState()
        {
            scene::Scene& scene = *m_scene;
            // JOINTS FIRST (teardown dependency order - the reverse of scene-start's
            // bodies-then-joints build): a joint referencing an entity whose body dies THIS
            // reconcile must be destroyed while both bodies are still alive, so the
            // surviving side gets its release-wake. (The foundation DestroyJoint is also
            // ID-safe against any ordering since the 2026-08-19 ASAN fix; this order keeps
            // the semantics right, that fix keeps any order memory-safe.)
            if (auto* joints = scene.GetSystem<JointComponentManager>())
            {
                // Joints reconcile on the full want/have compare (not just the own-entity
                // edge): an ACTIVE entity's joint must also drop when its explicit target
                // deactivates (that body is gone) and return when the target does.
                joints->ForEach(
                    [&](JointComponent& c, scene::EntityHandle e)
                    {
                        const bool eff = scene.IsEffectivelyActive(e);
                        c.simActive = eff;
                        const bool want = eff && JointTargetReady(c);
                        const bool have = c.joint.IsValid();
                        if (want == have)
                        {
                            return;
                        }
                        if (!want)
                        {
                            m_world->DestroyJoint(c.joint);
                            c.joint = JointId{};
                        }
                        else
                        {
                            CreateJointForEntity(c, e, /*logFailures=*/false);
                        }
                    });
            }
            if (auto* bodies = scene.GetSystem<RigidBodyComponentManager>())
            {
                bodies->ForEach(
                    [&](RigidBodyComponent& c, scene::EntityHandle e)
                    {
                        const bool eff = scene.IsEffectivelyActive(e);
                        if (eff == c.simActive)
                        {
                            return;
                        }
                        c.simActive = eff;
                        if (!eff)
                        {
                            if (c.body.IsValid())
                            {
                                m_world->DestroyBody(c.body);
                                c.body = BodyId{};
                            }
                        }
                        else if (!c.body.IsValid())
                        {
                            CreateBodyForEntity(c, e);
                        }
                    });
            }
            if (auto* characters = scene.GetSystem<CharacterComponentManager>())
            {
                characters->ForEach(
                    [&](CharacterComponent& c, scene::EntityHandle e)
                    {
                        const bool eff = scene.IsEffectivelyActive(e);
                        if (eff == c.simActive)
                        {
                            return;
                        }
                        c.simActive = eff;
                        if (!eff)
                        {
                            if (c.character.IsValid())
                            {
                                m_world->DestroyCharacter(c.character);
                                c.character = CharacterId{};
                            }
                        }
                        else if (!c.character.IsValid())
                        {
                            CreateCharacterForEntity(c, e);
                        }
                    });
            }
        }

        void BuildCharacters()
        {
            scene::Scene& scene = *m_scene;
            auto* characters = scene.GetSystem<CharacterComponentManager>();
            if (characters == nullptr)
            {
                return;
            }
            characters->ForEach(
                [&](CharacterComponent& c, scene::EntityHandle e)
                {
                    if (!scene.IsEffectivelyActive(e))
                    {
                        c.simActive = false; // built on the activation edge instead
                        return;
                    }
                    c.simActive = true;
                    CreateCharacterForEntity(c, e);
                });
        }

        // One entity's CharacterVirtual build (scene start + activation edge; re-creation
        // starts at the CURRENT pose with cleared momentum - v1 toggle semantics).
        void CreateCharacterForEntity(CharacterComponent& c, scene::EntityHandle e)
        {
            scene::Scene& scene = *m_scene;
            {
                {
                    Float3 position, scale;
                    Quaternion rotation;
                    if (!Decompose(scene.GetWorldMatrix(e), position, rotation, scale))
                    {
                        return;
                    }
                    CharacterDesc desc;
                    desc.capsuleRadius = c.radius;
                    desc.capsuleHalfHeight = c.halfHeight;
                    desc.maxSlopeDegrees = c.maxSlopeDegrees;
                    desc.mass = c.mass;
                    desc.maxStrength = c.maxStrength;
                    desc.stepUp = c.stepUp;
                    desc.stepDown = c.stepDown;
                    desc.position = position;
                    desc.userData = PackEntity(e); // lossless entity reverse-map (see PackEntity)
                    c.character = m_world->CreateCharacter(desc);
                    c.ground = CharacterGround::InAir;
                    c.prevPosition = c.currPosition = position;
                    c.moveVelocity = Float3{0, 0, 0};
                    c.jumpSpeed = 0.0f;
                    c.teleportPending = false;
                }
            }
        }

        void BuildBodies()
        {
            scene::Scene& scene = *m_scene;
            auto* bodies = scene.GetSystem<RigidBodyComponentManager>();
            if (bodies == nullptr)
            {
                return;
            }

            bodies->ForEach(
                [&](RigidBodyComponent& c, scene::EntityHandle e)
                {
                    // Effectively-inactive entities enter the world with NO body: the
                    // reconcile pass creates it on the activation edge (entity-active-state.md
                    // P3 - a scene that STARTS with the entity inactive never simulates it).
                    if (!scene.IsEffectivelyActive(e))
                    {
                        c.simActive = false;
                        return;
                    }
                    c.simActive = true;
                    CreateBodyForEntity(c, e);
                });
        }

        // One entity's body build (shared by scene start + the activation edge). Reads the
        // entity's CURRENT world transform - a body re-created on reactivation starts from
        // where the entity is NOW, with zero velocity (documented v1 toggle semantics).
        void CreateBodyForEntity(RigidBodyComponent& c, scene::EntityHandle e)
        {
            scene::Scene& scene = *m_scene;
            auto* colliders = scene.GetSystem<ColliderComponentManager>();
            {
                {
                    BodyDesc desc;
                    desc.motion = c.motion;
                    desc.layer = c.layer;
                    desc.friction = c.friction;
                    desc.restitution = c.restitution;
                    desc.linearDamping = c.linearDamping;
                    desc.angularDamping = c.angularDamping;
                    desc.isTrigger = c.isTrigger;
                    desc.group = c.collisionGroup;

                    // Reverse map: the owning entity handle, packed losslessly into the body user
                    // word (see PackEntity - unique by construction, unlike the guid's low bits).
                    desc.userData = PackEntity(e);

                    Float3 position, scale;
                    Quaternion rotation;
                    if (!Decompose(scene.GetWorldMatrix(e), position, rotation, scale))
                    {
                        return;
                    }

                    ShapeDesc own;
                    own.kind = c.shape;
                    own.halfExtents = c.halfExtents;
                    own.radius = c.radius;
                    own.halfHeight = c.halfHeight;
                    own.planeHalfExtent = c.planeHalfExtent;
                    if (c.shape == ShapeKind::Cooked)
                    {
                        CollisionShape* cooked = c.collisionShape.Get();
                        if (cooked == nullptr)
                        {
                            LOG_WARNING(u8"Physics",
                                                 u8"'{}': cooked shape has no collision-shape "
                                                 u8"resource - body skipped",
                                                 scene.GetEntityName(e));
                            return;
                        }
                        own.cooked = cooked->Blob();
                        own.scale = scale; // cooked geometry is authored unit-scale
                    }
                    desc.shapes.PushBack(own);

                    // Hierarchy compounding: descendant ColliderComponents fold in at their
                    // offset relative to THIS entity (captured at start).
                    if (colliders != nullptr)
                    {
                        const Float4x4 bodyInverse = Inverse(scene.GetWorldMatrix(e));
                        colliders->ForEach(
                            [&](ColliderComponent& extra, scene::EntityHandle child)
                            {
                                if (!IsDescendantOf(scene, child, e))
                                {
                                    return;
                                }
                                Float3 lp, ls;
                                Quaternion lr;
                                if (!Decompose(scene.GetWorldMatrix(child) * bodyInverse, lp, lr,
                                               ls))
                                {
                                    return;
                                }
                                ShapeDesc shape;
                                shape.kind = extra.shape;
                                shape.halfExtents = extra.halfExtents;
                                shape.radius = extra.radius;
                                shape.halfHeight = extra.halfHeight;
                                shape.planeHalfExtent = extra.planeHalfExtent;
                                if (extra.shape == ShapeKind::Cooked)
                                {
                                    CollisionShape* cooked = extra.collisionShape.Get();
                                    if (cooked == nullptr)
                                    {
                                        return;
                                    }
                                    shape.cooked = cooked->Blob();
                                    shape.scale = ls;
                                }
                                shape.localPosition = lp;
                                shape.localRotation = lr;
                                desc.shapes.PushBack(shape);
                            });
                    }

                    desc.position = position;
                    desc.rotation = rotation;

                    // A referenced PhysicalMaterial wins over the inline surface fields.
                    if (PhysicalMaterial* material = c.material.Get())
                    {
                        desc.friction = material->friction;
                        desc.restitution = material->restitution;
                        desc.density = material->density;
                    }

                    c.body = m_world->CreateBody(desc);
                    c.prevPosition = c.currPosition = position;
                    c.prevRotation = c.currRotation = rotation;
                    if (!c.body.IsValid())
                    {
                        LOG_WARNING(u8"Physics", u8"body creation failed for '{}'",
                                             scene.GetEntityName(e));
                    }
                }
            }
        }

        // Resolve every drained contact's packed user words back to live entities and push
        // the result to each registered listener. A side that doesn't resolve (destroyed
        // body / stale slot) is delivered as an invalid handle; a contact with neither side
        // live is dropped.
        void DispatchContacts()
        {
            if (m_listeners == nullptr || m_listeners->IsEmpty() || m_scene == nullptr)
            {
                return;
            }
            for (const ContactEvent& event : m_events)
            {
                const scene::EntityHandle a = UnpackEntity(event.userA);
                const scene::EntityHandle b = UnpackEntity(event.userB);
                EntityContact contact;
                contact.kind = event.kind;
                contact.scene = m_scene;
                contact.a = m_scene->IsValid(a) ? a : scene::EntityHandle::Invalid();
                contact.b = m_scene->IsValid(b) ? b : scene::EntityHandle::Invalid();
                if (!contact.a.IsAssigned() && !contact.b.IsAssigned())
                {
                    continue;
                }
                contact.point = event.point;
                contact.normal = event.normal;
                contact.speed = event.speed;
                for (IContactListener* listener : *m_listeners)
                {
                    if (listener != nullptr)
                    {
                        listener->OnContact(contact);
                    }
                }
            }
        }

        [[nodiscard]] static bool IsDescendantOf(scene::Scene& scene, scene::EntityHandle child,
                                                 scene::EntityHandle ancestor)
        {
            for (scene::EntityHandle e = child; e.IsAssigned(); e = scene.GetParent(e))
            {
                if (e == ancestor)
                {
                    return true;
                }
            }
            return false;
        }

        scene::Scene* m_scene = nullptr; // set by OnSceneCreate
        PhysicsSceneSettings m_settings;
        UniquePtr<PhysicsWorld> m_world;
        RayHit m_lastHit;             // last ScenePhysics.rayCast result on this scene
        bool m_lastHitValid = false;
        Array<ContactEvent> m_events;
        const Array<IContactListener*>* m_listeners = nullptr; // owned by the subsystem
    };

    // The runtime subsystem: contributes the managers + system to the scene composition
    // and drives render-frame interpolation + debug draw with the engine's fixed alpha.
    // THE physics manager set for a scene - injected by the subsystem at runtime AND by headless
    // scene consumers (Engine.SceneSurface). Runtime wiring (contact listeners) stays with the
    // subsystem. Add a manager => bump the SceneSurface tripwire (engine::kSceneSystemCount).
    inline void AddPhysicsSceneManagers(scene::Scene& scene)
    {
        scene.AddSystem<RigidBodyComponentManager>();
        scene.AddSystem<ColliderComponentManager>();
        scene.AddSystem<JointComponentManager>();
        scene.AddSystem<CharacterComponentManager>();
        scene.AddSystem<PhysicsSceneSystem>(); // carries the per-scene settings block
    }

    class PhysicsSubsystem final : public foundation::runtime::Subsystem, public scene::ISceneObserver
    {
    public:
        /// Register a consumer of resolved contacts (the script subsystem). Duplicates are
        /// ignored; every per-scene world dispatches to the shared list at its physics tick.
        void RegisterContactListener(IContactListener* listener)
        {
            if (listener == nullptr)
            {
                return;
            }
            for (IContactListener* existing : m_contactListeners)
            {
                if (existing == listener)
                {
                    return;
                }
            }
            m_contactListeners.PushBack(listener);
        }
        void UnregisterContactListener(IContactListener* listener)
        {
            for (usize i = 0; i < m_contactListeners.Size(); ++i)
            {
                if (m_contactListeners[i] == listener)
                {
                    m_contactListeners.RemoveAt(i);
                    return;
                }
            }
        }

        // BEFORE the scene subsystem (-500): the interpolation's local-transform writes
        // must land before Scene::Update recomputes world matrices, or rendering (which
        // extracts world matrices) would lag the physics poses by a frame.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -600; }

        void OnSystemsReady(scene::Scene& scene) override
        {
            PhysicsSceneSystem* system = scene.GetSystem<PhysicsSceneSystem>();
            system->SetContactListeners(&m_contactListeners); // shared list, stable address
            m_systems.PushBack(SceneEntry{&scene, system});
        }
        void OnDestroying(scene::Scene& scene) override
        {
            for (usize i = 0; i < m_systems.Size(); ++i)
            {
                if (m_systems[i].scene == &scene)
                {
                    m_systems.RemoveAt(i);
                    return;
                }
            }
        }

        // Defined in SubsystemImpl.cpp: interpolation + debug wireframes (render dep)
        // + retargeting the script binding at the first live world.
        void Update(f32 deltaTime) override;

    protected:
        void OnInit() override { RegisterPhysicsComponentReflection(); }
        void OnReady() override
        {
            if (foundation::runtime::Context* context = GetContext())
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
            if (foundation::runtime::Context* context = GetContext())
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
            PhysicsSceneSystem* system = nullptr;
        };
        [[nodiscard]] Span<const SceneEntry> Systems() const noexcept
        {
            return Span<const SceneEntry>{m_systems.Data(), m_systems.Size()};
        }

    private:
        Array<SceneEntry> m_systems;
        Array<IContactListener*> m_contactListeners; // consumers of resolved contacts
    };

    // Scene-bound physics handle - the reflected, per-scene answer to the static `Physics` facade.
    // OPTION 1 factory shape (spec Section 12, same ruling as RigidBody.of(entity)): `ScenePhysics.of(
    // scene)` returns a handle whose ops act on THAT scene's world (its PhysicsSceneSystem), not the
    // first started scene. A plain value carrying the scene pointer (returned by value - concrete
    // type, cross-backend, no hook slot); a null scene / a scene with no physics system is a safe
    // no-op. The hit-point accessors of the static facade are a follow-up (they need per-scene state).
    struct ScenePhysics
    {
        scene::Scene* scene = nullptr;

        [[nodiscard]] PhysicsSceneSystem* System() const
        {
            return scene != nullptr ? scene->GetSystem<PhysicsSceneSystem>() : nullptr;
        }
        [[nodiscard]] PhysicsWorld* World() const
        {
            PhysicsSceneSystem* system = System();
            return (system != nullptr) ? system->World() : nullptr;
        }

        void setGravity(f32 x, f32 y, f32 z)
        {
            if (PhysicsWorld* world = World())
            {
                world->SetGravity(Float3{x, y, z});
            }
        }
        [[nodiscard]] f32 gravityY() const
        {
            PhysicsWorld* world = World();
            return world != nullptr ? world->Gravity().y : 0.0f;
        }
        // Ray against THIS scene's world; the hit distance, or -1 on a miss. Records the hit on the
        // scene's PhysicsSceneSystem so the hit* accessors below can answer the follow-up questions.
        [[nodiscard]] f32 rayCast(f32 fromX, f32 fromY, f32 fromZ, f32 dirX, f32 dirY, f32 dirZ,
                                  f32 maxDistance) const
        {
            PhysicsSceneSystem* system = System();
            PhysicsWorld* world = (system != nullptr) ? system->World() : nullptr;
            if (world == nullptr)
            {
                if (system != nullptr)
                {
                    system->SetLastHit(RayHit{}, false);
                }
                return -1.0f;
            }
            RayHit hit;
            const bool ok = world->RayCast(Float3{fromX, fromY, fromZ}, Float3{dirX, dirY, dirZ},
                                           maxDistance, hit);
            system->SetLastHit(hit, ok);
            return ok ? hit.fraction * maxDistance : -1.0f;
        }

        // Hit details of the last rayCast on THIS scene (0 on a miss / no world).
        [[nodiscard]] f32 hitX() const
        {
            PhysicsSceneSystem* s = System();
            return s != nullptr && s->LastHitValid() ? s->LastHit().position.x : 0.0f;
        }
        [[nodiscard]] f32 hitY() const
        {
            PhysicsSceneSystem* s = System();
            return s != nullptr && s->LastHitValid() ? s->LastHit().position.y : 0.0f;
        }
        [[nodiscard]] f32 hitZ() const
        {
            PhysicsSceneSystem* s = System();
            return s != nullptr && s->LastHitValid() ? s->LastHit().position.z : 0.0f;
        }
        [[nodiscard]] f32 hitNormalX() const
        {
            PhysicsSceneSystem* s = System();
            return s != nullptr && s->LastHitValid() ? s->LastHit().normal.x : 0.0f;
        }
        [[nodiscard]] f32 hitNormalY() const
        {
            PhysicsSceneSystem* s = System();
            return s != nullptr && s->LastHitValid() ? s->LastHit().normal.y : 0.0f;
        }
        [[nodiscard]] f32 hitNormalZ() const
        {
            PhysicsSceneSystem* s = System();
            return s != nullptr && s->LastHitValid() ? s->LastHit().normal.z : 0.0f;
        }
        // Material slot of the hit face (cooked triangle meshes; 0 otherwise).
        [[nodiscard]] f32 hitSurface() const
        {
            PhysicsSceneSystem* s = System();
            return s != nullptr && s->LastHitValid() ? static_cast<f32>(s->LastHit().surface) : 0.0f;
        }
        // The ENTITY the last rayCast hit (from the body's packed user word), or an invalid Entity on
        // a miss / a body whose entity is no longer live.
        [[nodiscard]] foundation::script::Entity rayHitEntity() const
        {
            PhysicsSceneSystem* s = System();
            if (s == nullptr || !s->LastHitValid() || scene == nullptr)
            {
                return foundation::script::Entity{};
            }
            const scene::EntityHandle handle = UnpackEntity(s->LastHit().userData);
            if (!scene->IsValid(handle))
            {
                return foundation::script::Entity{};
            }
            foundation::script::Entity entity;
            entity.scene = scene;
            entity.entityIndex = handle.index;
            entity.entityGeneration = handle.generation;
            return entity;
        }
        // Impulse on the body the last rayCast hit (no-op on a miss).
        void impulseOnHit(f32 x, f32 y, f32 z)
        {
            PhysicsSceneSystem* s = System();
            PhysicsWorld* world = World();
            if (s == nullptr || world == nullptr || !s->LastHitValid())
            {
                return;
            }
            world->AddImpulse(s->LastHit().body, Float3{x, y, z});
        }
        [[nodiscard]] f32 bodyCount() const
        {
            PhysicsWorld* world = World();
            return world != nullptr ? static_cast<f32>(world->BodyCount()) : 0.0f;
        }

        // Apply an impulse to `entity`'s rigid body in THIS scene (the scriptable-impulse gameplay
        // op - a genuine WORLD operation, so it lives on scene.physics keyed by the entity, not on
        // the component data which cannot reach the world). No-op if the entity has no rigid body or
        // its body is not yet created. Fixes the "no scriptable impulse on a dynamic body" gap.
        void applyImpulse(foundation::script::Entity entity, f32 x, f32 y, f32 z)
        {
            PhysicsWorld* world = World();
            if (world == nullptr || scene == nullptr)
            {
                return;
            }
            RigidBodyComponentManager* bodies = scene->GetSystem<RigidBodyComponentManager>();
            RigidBodyComponent* body = (bodies != nullptr) ? bodies->Get(entity.Handle()) : nullptr;
            if (body != nullptr)
            {
                world->AddImpulse(body->body, Float3{x, y, z});
            }
        }

        // The OPTION 1 factory: ScenePhysics.of(scene). Argument is the bound Scene facade.
        [[nodiscard]] static ScenePhysics of(foundation::script::Scene sceneHandle)
        {
            return ScenePhysics{sceneHandle.scene};
        }
    };

    /// Registers the physics script surface (ScenePhysics.of + the reflected components) into the
    /// global registry + the behavior prelude. (Kept the historical name; there is no static facade.)
    void RegisterPhysicsScriptFacade();
}
