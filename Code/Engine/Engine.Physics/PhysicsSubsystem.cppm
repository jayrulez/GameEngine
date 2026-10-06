// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Physics - the `engine.physics` module.
//
// Scene integration: a PhysicsSceneSystem per scene owns its
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
import engine.domain;
import foundation.profiler;
import foundation.runtime;
import foundation.scene;
import engine.scene;
import foundation.script;
import foundation.script.facades; // foundation::script::Entity (the raycast/contact hit entity)
import foundation.physics;
import foundation.heightfield;
// NOTE: no render imports HERE - the debug-draw path lives in SubsystemImpl.cpp (a module
// implementation unit). Keeping heavyweight imports out of the interface matters for
// GCC's module loader (-fno-module-lazy consumers force-load the whole import graph).

using namespace foundation::core;
using namespace foundation::physics;
using namespace engine::scene;
using foundation::heightfield::Heightfield;

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

    [[nodiscard]] inline bool IsDescendantOf(scene::Scene& scene, scene::EntityHandle child,
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

    /// The body a RigidBodyComponent makes on `entity`: its settings, the entity's world pose, its
    /// own shape and the ColliderComponents under it (a compound at their offsets). Heightfield
    /// samples are copied into `heightBuffers`, which the desc points into and which must outlive
    /// it. False (logged) when the transform does not decompose or a shape's resource is missing.
    /// Body creation and the static geometry the navigation bake reads both describe bodies here.
    [[nodiscard]] bool DescribeBody(scene::Scene& scene, RigidBodyComponent& c, scene::EntityHandle entity,
                                    BodyDesc& out, Array<Array<f32>>& heightBuffers);

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

    class PhysicsSceneSystem final : public scene::SceneSystem, public scene::ISceneRayQuery
    {
    public:
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }

        // Rays against the world's solid bodies for systems that do not depend on physics (foot IK
        // finds the ground under a foot): triggers are skipped, a checkpoint is not a floor.
        [[nodiscard]] scene::ISceneRayQuery* AsRayQuery() noexcept override { return this; }
        [[nodiscard]] bool CastRay(Float3 origin, Float3 direction, f32 maxDistance, u32 groupMask,
                                   scene::SceneRayHit& out) override
        {
            RayHit hit;
            if (m_world.Get() == nullptr ||
                !m_world->RayCast(origin, direction, maxDistance, hit, groupMask, true))
            {
                return false;
            }
            out.distance = hit.fraction * maxDistance;
            out.position = hit.position;
            out.normal = hit.normal;
            return true;
        }

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
            m_world = MakeUnique<PhysicsWorld>(DefaultAllocator(), DefaultAllocator(), settings);
            BuildBodies();
            BuildStaticCapsules();
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
                        // The queue is RUN-scoped: an impulse queued in the last frame before Stop
                        // must not fire as an unexplained kick at the next Play.
                        c.pendingImpulse = Float3{0, 0, 0};
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
            m_pendingCapsuleSources.Clear(); // the capsule bodies drop with the world
            m_capsuleWait = 0.0f;
            m_warnedCapsules = false;
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

            RetryStaticCapsules(fixedDeltaTime); // a source still resolving at start, solid once ready

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
                            c.driveVelocity = Float3{0, 0, 0};
                            c.velocity = Float3{0, 0, 0};
                            c.groundNormal = Float3{0, 1, 0};
                            c.jumpSpeed = 0.0f;
                            c.launchPending = false;
                            c.prevPosition = c.teleportTo;
                            c.currPosition = c.teleportTo;
                            c.ground = CharacterGround::InAir;
                            c.teleportPending = false;
                            return;
                        }
                        const Float3 current = m_world->CharacterVelocity(c.character);
                        Float3 velocity{c.moveVelocity.x, 0.0f, c.moveVelocity.z};
                        if (c.driving)
                        {
                            velocity = c.driveVelocity;
                            c.launchPending = false;
                            c.jumpSpeed = 0.0f;
                        }
                        else if (c.launchPending)
                        {
                            // A launch sets the vertical speed wherever the character is.
                            velocity.y = c.launchSpeed;
                            c.launchPending = false;
                            c.jumpSpeed = 0.0f;
                        }
                        else if (c.ground == CharacterGround::OnGround)
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
                        c.groundNormal = m_world->CharacterGroundNormal(c.character);
                        // The motion the sweep allowed, not the velocity asked for: a wall or a
                        // slope bends it, and a script integrating momentum needs what happened.
                        const Float3 moved = c.currPosition - c.prevPosition;
                        c.velocity = moved * (1.0f / fixedDeltaTime);
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

        // Reconcile the Jolt world against effective-active
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
                            // A queued impulse lives only until the assembly right after it was
                            // queued (spawn-then-launch). If the body STILL is not valid here (build
                            // failed, or the entity is inactive), drop the queue - otherwise a
                            // script calling applyImpulse every frame accumulates an unbounded
                            // launch that fires whenever the body finally appears.
                            if (!c.body.IsValid())
                            {
                                c.pendingImpulse = Float3{0, 0, 0};
                            }
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
                            // Impulses queued while active die with the body; re-activation must
                            // not launch a sum accumulated across the inactive window.
                            c.pendingImpulse = Float3{0, 0, 0};
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
                    c.launchPending = false;
                    c.teleportPending = false;
                }
            }
        }

        // Static content without an entity per piece (a forest's trunks): every system that is an
        // IStaticColliderSource gives its capsules, one static body each in its group, owned by no
        // entity (Specs/vegetation-colliders.md). A source not ready yet is asked again each step.
        void BuildStaticCapsules()
        {
            m_scene->ForEachSystem(
                [&](scene::SceneSystem& system)
                {
                    if (scene::IStaticColliderSource* source = system.AsStaticColliderSource())
                    {
                        CollectCapsules(*source);
                    }
                });
        }

        void CollectCapsules(scene::IStaticColliderSource& source)
        {
            Array<scene::StaticCapsule> capsules;
            if (!source.CollectStaticCapsules(*m_scene, capsules))
            {
                m_pendingCapsuleSources.PushBack(&source);
                return;
            }
            for (const scene::StaticCapsule& capsule : capsules)
            {
                CreateCapsuleBody(capsule);
            }
        }

        void RetryStaticCapsules(f32 dt)
        {
            if (m_pendingCapsuleSources.IsEmpty())
            {
                return;
            }
            Array<scene::IStaticColliderSource*> pending = Move(m_pendingCapsuleSources);
            m_pendingCapsuleSources.Clear();
            for (scene::IStaticColliderSource* source : pending)
            {
                CollectCapsules(*source);
            }
            m_capsuleWait += dt;
            if (!m_pendingCapsuleSources.IsEmpty() && m_capsuleWait > 5.0f && !m_warnedCapsules)
            {
                m_warnedCapsules = true;
                LOG_WARNING(u8"Physics",
                            u8"{} static collider source(s) still not ready after 5 s: their content "
                            u8"is not solid until it is",
                            m_pendingCapsuleSources.Size());
            }
        }

        // An upright capsule from `foot` up `height` (the whole capsule; the cylinder between the
        // caps is height - 2 radius, at least 0: a short one is a sphere sitting on the foot).
        void CreateCapsuleBody(const scene::StaticCapsule& capsule)
        {
            if (capsule.radius <= 0.0f)
            {
                return;
            }
            BodyDesc desc;
            desc.motion = MotionKind::Static;
            desc.layer = PhysicsLayer::Static;
            desc.group = capsule.group;
            ShapeDesc shape;
            const f32 cylinder = Max(capsule.height - 2.0f * capsule.radius, 0.0f);
            shape.kind = cylinder > 0.0f ? ShapeKind::Capsule : ShapeKind::Sphere;
            shape.radius = capsule.radius;
            shape.halfHeight = cylinder * 0.5f;
            desc.shapes.PushBack(shape);
            desc.position = capsule.foot + Float3{0.0f, capsule.radius + cylinder * 0.5f, 0.0f};
            (void)m_world->CreateBody(desc);
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
                    // reconcile pass creates it on the activation edge (a scene that
                    // STARTS with the entity inactive never simulates it).
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
            {
                {
                    // Heightfield sample buffers kept alive until CreateBody, where Jolt copies them.
                    Array<Array<f32>> heightBuffers;
                    BodyDesc desc;
                    if (!DescribeBody(scene, c, e, desc, heightBuffers))
                    {
                        return;
                    }
                    const Float3 position = desc.position;
                    const Quaternion rotation = desc.rotation;

                    c.body = m_world->CreateBody(desc);
                    c.prevPosition = c.currPosition = position;
                    c.prevRotation = c.currRotation = rotation;
                    // Flush an impulse queued before the body existed (spawn-then-launch, e.g. a
                    // thrown paper): applyImpulse accumulated it on the component; apply it now once.
                    if (c.body.IsValid() &&
                        (c.pendingImpulse.x != 0.0f || c.pendingImpulse.y != 0.0f ||
                         c.pendingImpulse.z != 0.0f))
                    {
                        m_world->AddImpulse(c.body, c.pendingImpulse);
                        c.pendingImpulse = Float3{0, 0, 0};
                    }
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

        scene::Scene* m_scene = nullptr; // set by OnSceneCreate
        PhysicsSceneSettings m_settings;
        UniquePtr<PhysicsWorld> m_world;
        Array<ContactEvent> m_events;
        Array<scene::IStaticColliderSource*> m_pendingCapsuleSources; // not ready at the last ask
        f32 m_capsuleWait = 0.0f;   // how long they have been waited for (s)
        bool m_warnedCapsules = false;
        const Array<IContactListener*>* m_listeners = nullptr; // owned by the subsystem
    };

    // The runtime subsystem: contributes the managers + system to the scene composition
    // and drives render-frame interpolation + debug draw with the engine's fixed alpha.
    // THE physics manager set for a scene - injected by the subsystem at runtime AND by headless
    // scene consumers (Engine.Composition). Runtime wiring (contact listeners) stays with the
    // subsystem. A manager added here reaches the composition (Engine.Composition) automatically.
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
    // OPTION 1 factory shape (mirrors RigidBody.of(entity)): `ScenePhysics.of(
    // scene)` returns a handle whose ops act on THAT scene's world (its PhysicsSceneSystem), not the
    // first started scene. A plain value carrying the scene pointer (returned by value - concrete
    // type, cross-backend, no hook slot); a null scene / a scene with no physics system is a safe
    // no-op. The static facade has no hit-point accessors (they would need per-scene state).
    // An EXPLICIT ray-hit result: the result travels BY VALUE from
    // the rayCast that produced it, like every other bound value handle. Carries the scene so
    // entity()/impulse() resolve live state at CALL time (never cached pointers).
    struct RayCastHit
    {
        scene::Scene* scene = nullptr;
        foundation::physics::BodyId body;
        u64 packedEntity = 0;
        bool hit = false;
        f32 distance = -1.0f; // world units to the hit; -1 on a miss
        Float3 position{0.0f, 0.0f, 0.0f};
        Float3 normal{0.0f, 0.0f, 0.0f};
        i32 surface = 0; // material slot of the hit face (cooked triangle meshes; 0 otherwise)

        // The ENTITY the ray hit (unpacked from the body user word), or invalid on a miss /
        // a body whose entity is no longer live.
        [[nodiscard]] foundation::script::Entity entity() const
        {
            if (!hit || scene == nullptr)
            {
                return foundation::script::Entity{};
            }
            const scene::EntityHandle handle = UnpackEntity(packedEntity);
            if (!scene->IsValid(handle))
            {
                return foundation::script::Entity{};
            }
            foundation::script::Entity e;
            e.scene = scene;
            e.entityIndex = handle.index;
            e.entityGeneration = handle.generation;
            return e;
        }

        // Impulse on the hit body (no-op on a miss / dead world). Resolves the world at call
        // time through the carried scene - the hit stores no world pointer.
        void impulse(f32 x, f32 y, f32 z) const
        {
            if (!hit || scene == nullptr)
            {
                return;
            }
            PhysicsSceneSystem* system = scene->GetSystem<PhysicsSceneSystem>();
            PhysicsWorld* world = (system != nullptr) ? system->World() : nullptr;
            if (world != nullptr)
            {
                world->AddImpulse(body, Float3{x, y, z});
            }
        }
    };

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

        void setGravity(f32 x, f32 y, f32 z) { setGravity(Float3{x, y, z}); }
        void setGravity(Float3 gravity)
        {
            if (PhysicsWorld* world = World())
            {
                world->SetGravity(gravity);
            }
        }
        [[nodiscard]] Float3 gravity() const
        {
            PhysicsWorld* world = World();
            return world != nullptr ? world->Gravity() : Float3{0.0f, 0.0f, 0.0f};
        }
        [[nodiscard]] f32 gravityY() const { return gravity().y; }

        // Ray against THIS scene's world -> an EXPLICIT RayCastHit (hit/distance/position/
        // normal/surface + entity() + impulse()); RayCastHit.hit == false on a miss. No
        // stored last-hit state exists anymore. `groupMask`: bit g = consider collision group g.
        [[nodiscard]] RayCastHit rayCast(Float3 from, Float3 direction, f32 maxDistance,
                                         u32 groupMask) const
        {
            RayCastHit result;
            result.scene = scene;
            PhysicsWorld* world = World();
            if (world == nullptr)
            {
                return result;
            }
            RayHit hit;
            if (world->RayCast(from, direction, maxDistance, hit, groupMask))
            {
                result.hit = true;
                result.body = hit.body;
                result.packedEntity = hit.userData;
                result.distance = hit.fraction * maxDistance;
                result.position = hit.position;
                result.normal = hit.normal;
                result.surface = static_cast<i32>(hit.surface);
            }
            return result;
        }
        [[nodiscard]] RayCastHit rayCast(Float3 from, Float3 direction, f32 maxDistance) const
        {
            return rayCast(from, direction, maxDistance, 0xFFFFFFFFu);
        }
        [[nodiscard]] RayCastHit rayCast(f32 fromX, f32 fromY, f32 fromZ, f32 dirX, f32 dirY,
                                         f32 dirZ, f32 maxDistance) const
        {
            return rayCast(Float3{fromX, fromY, fromZ}, Float3{dirX, dirY, dirZ}, maxDistance,
                           0xFFFFFFFFu);
        }

        // A swept SPHERE (radius) from `from` along `direction` up to maxDistance -> the CLOSEST hit,
        // as a RayCastHit (hit == false on a miss). Like rayCast but with a volume - the ray that would
        // slip through a gap a fat projectile cannot. `direction` must be UNIT length, like rayCast:
        // `distance` scales by its length otherwise.
        [[nodiscard]] RayCastHit sphereCast(Float3 from, Float3 direction, f32 maxDistance,
                                            f32 radius, u32 groupMask) const
        {
            RayCastHit result;
            result.scene = scene;
            PhysicsWorld* world = World();
            if (world == nullptr)
            {
                return result;
            }
            QueryShape shape;
            shape.kind = ShapeKind::Sphere;
            shape.radius = radius;
            RayHit hit;
            if (world->ShapeCast(shape, from, Quaternion::Identity, direction, maxDistance, hit,
                                 groupMask))
            {
                result.hit = true;
                result.body = hit.body;
                result.packedEntity = hit.userData;
                result.distance = hit.fraction * maxDistance;
                result.position = hit.position;
                result.normal = hit.normal;
                result.surface = static_cast<i32>(hit.surface);
            }
            return result;
        }
        [[nodiscard]] RayCastHit sphereCast(Float3 from, Float3 direction, f32 maxDistance,
                                            f32 radius) const
        {
            return sphereCast(from, direction, maxDistance, radius, 0xFFFFFFFFu);
        }
        [[nodiscard]] RayCastHit sphereCast(f32 fromX, f32 fromY, f32 fromZ, f32 dirX, f32 dirY,
                                            f32 dirZ, f32 maxDistance, f32 radius) const
        {
            return sphereCast(Float3{fromX, fromY, fromZ}, Float3{dirX, dirY, dirZ}, maxDistance,
                              radius, 0xFFFFFFFFu);
        }

        // The body NEAREST to `center` whose shape overlaps a SPHERE (radius) there, filtered to
        // `groupMask` (bit g = include collision group g; ~0 = all) -> a RayCastHit (hit == false if
        // none). The script surface returns one handle rather than a list, so this gives the nearest -
        // exactly what "aim at / act on the closest thing in range" needs; the group mask does the
        // category filtering (e.g. only delivery-zone triggers). `result.position` is the body origin.
        [[nodiscard]] RayCastHit nearestOverlap(Float3 center, f32 radius, u32 groupMask) const
        {
            RayCastHit result;
            result.scene = scene;
            PhysicsWorld* world = World();
            if (world == nullptr)
            {
                return result;
            }
            QueryShape shape;
            shape.kind = ShapeKind::Sphere;
            shape.radius = radius;
            Array<foundation::physics::BodyId> bodies;
            world->ShapeOverlap(shape, center, Quaternion::Identity, bodies, groupMask);
            f32 bestSq = kFloatMax; // best body-ORIGIN distance (see the semantics note above)
            for (const foundation::physics::BodyId& b : bodies)
            {
                Float3 pos;
                Quaternion rot;
                world->GetBodyTransform(b, pos, rot);
                const f32 dx = pos.x - center.x, dy = pos.y - center.y, dz = pos.z - center.z;
                const f32 d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestSq)
                {
                    bestSq = d2;
                    result.hit = true;
                    result.body = b;
                    result.packedEntity = world->UserData(b);
                    result.position = pos;
                    result.normal = Float3{0.0f, 0.0f, 0.0f};
                    result.surface = 0;
                }
            }
            if (result.hit)
            {
                result.distance = Sqrt(bestSq);
            }
            return result;
        }
        [[nodiscard]] RayCastHit nearestOverlap(Float3 center, f32 radius) const
        {
            return nearestOverlap(center, radius, 0xFFFFFFFFu);
        }
        [[nodiscard]] RayCastHit nearestOverlap(f32 x, f32 y, f32 z, f32 radius, i32 groupMask) const
        {
            return nearestOverlap(Float3{x, y, z}, radius, static_cast<u32>(groupMask));
        }

        // ALL entities overlapping a SPHERE (radius) at `center`, filtered to `groupMask` (bit g =
        // include collision group g; ~0 = all) -> a native array the script walks with `.length`/`[]`
        // (AngelScript) or `#`/`ipairs` (Luau). This is the full set (nearestOverlap is the convenience
        // for "just the closest"). RESOLVED Entities, not packed u64 words: a Lua table element is an
        // f64 and a packed handle exceeds 2^53. Since-destroyed bodies are
        // skipped, so every element is a live entity.
        [[nodiscard]] Array<foundation::script::Entity> overlapSphere(Float3 center, f32 radius,
                                                                     u32 groupMask) const
        {
            Array<foundation::script::Entity> result;
            PhysicsWorld* world = World();
            if (world == nullptr || scene == nullptr)
            {
                return result;
            }
            QueryShape shape;
            shape.kind = ShapeKind::Sphere;
            shape.radius = radius;
            Array<foundation::physics::BodyId> bodies;
            world->ShapeOverlap(shape, center, Quaternion::Identity, bodies, groupMask);
            for (const foundation::physics::BodyId& b : bodies)
            {
                const scene::EntityHandle handle = UnpackEntity(world->UserData(b));
                if (!scene->IsValid(handle))
                {
                    continue;
                }
                foundation::script::Entity e;
                e.scene = scene;
                e.entityIndex = handle.index;
                e.entityGeneration = handle.generation;
                result.PushBack(e);
            }
            return result;
        }
        [[nodiscard]] Array<foundation::script::Entity> overlapSphere(Float3 center, f32 radius) const
        {
            return overlapSphere(center, radius, 0xFFFFFFFFu);
        }
        [[nodiscard]] Array<foundation::script::Entity> overlapSphere(f32 x, f32 y, f32 z, f32 radius,
                                                                     i32 groupMask) const
        {
            return overlapSphere(Float3{x, y, z}, radius, static_cast<u32>(groupMask));
        }

        [[nodiscard]] f32 bodyCount() const
        {
            PhysicsWorld* world = World();
            return world != nullptr ? static_cast<f32>(world->BodyCount()) : 0.0f;
        }

        // Apply an impulse to `entity`'s rigid body in THIS scene (the scriptable-impulse gameplay
        // op - a genuine WORLD operation, so it lives on scene.physics keyed by the entity, not on
        // the component data which cannot reach the world). If the body is not yet created (a prefab
        // spawned + launched THIS frame), the impulse is queued on the component and applied at the
        // next assembly; a queue that survives an assembly without gaining a body is dropped (the
        // reconcile pass clears it), and Stop clears it too - it never outlives the run.
        void applyImpulse(foundation::script::Entity entity, f32 x, f32 y, f32 z)
        {
            applyImpulse(entity, Float3{x, y, z});
        }
        void applyImpulse(foundation::script::Entity entity, Float3 impulse)
        {
            PhysicsWorld* world = World();
            if (world == nullptr || scene == nullptr)
            {
                return;
            }
            RigidBodyComponent* body = (scene->GetSystem<RigidBodyComponentManager>() != nullptr)
                                           ? scene->GetSystem<RigidBodyComponentManager>()->Get(
                                                 entity.Handle())
                                           : nullptr;
            if (body == nullptr)
            {
                return;
            }
            if (body->body.IsValid())
            {
                world->AddImpulse(body->body, impulse);
            }
            else
            {
                // Body not created yet (a prefab spawned + launched THIS frame): queue the impulse on
                // the component; CreateBodyForEntity applies it when the Jolt body is made next assembly.
                body->pendingImpulse += impulse;
            }
        }

        // ---- the entity's character, as Sedulous's PhysicsFacade has it (the same ops as
        // CharacterComponent.of(entity); no-ops for an entity without a character) ----
        void moveCharacter(foundation::script::Entity entity, f32 velocityX, f32 velocityZ) const
        {
            if (CharacterComponent* character = Character(entity))
            {
                character->move(velocityX, velocityZ);
            }
        }
        void jumpCharacter(foundation::script::Entity entity, f32 speed) const
        {
            if (CharacterComponent* character = Character(entity))
            {
                character->jump(speed);
            }
        }
        void launchCharacter(foundation::script::Entity entity, f32 speed) const
        {
            if (CharacterComponent* character = Character(entity))
            {
                character->launch(speed);
            }
        }
        void setCharacterPosition(foundation::script::Entity entity, Float3 position) const
        {
            if (CharacterComponent* character = Character(entity))
            {
                character->setPosition(position);
            }
        }
        [[nodiscard]] bool isCharacterGrounded(foundation::script::Entity entity) const
        {
            CharacterComponent* character = Character(entity);
            return character != nullptr && character->grounded();
        }
        [[nodiscard]] CharacterComponent* Character(foundation::script::Entity entity) const
        {
            if (scene == nullptr || entity.scene != scene)
            {
                return nullptr;
            }
            auto* characters = scene->GetSystem<CharacterComponentManager>();
            return characters != nullptr ? characters->Get(entity.Handle()) : nullptr;
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

export namespace engine::physics
{
    /// This domain's declaration (engine-composition.md D4): what it brings to a scene, to
    /// reflection, to the script surface and which resource modules come with it. Defined in the
    /// implementation unit (one instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& PhysicsDomain() noexcept;
}
