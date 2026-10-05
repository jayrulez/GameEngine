// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Animation - the `:ik` partition.
///
/// Inverse kinematics on scene entities (inverse-kinematics.md P2): TwoBoneIkComponent (a leg, an
/// arm) and AimIkComponent (a head, a spine), each on an entity under the animated model (or on
/// it), driving the nearest ancestor-or-self animator (an animation graph or a skeletal animation
/// component). Each component is a pose modifier on that animator's player: its manager runs in
/// PostUpdate before the animation managers, resolves the chain by bone names, eases the weight,
/// turns the targets into the skeleton's model space (the first mesh entity's world, composed
/// fresh: the cached world matrices update after PostUpdate) and leaves the solve to the player,
/// which runs it between the pose and the palette.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Profiler/Profiler.h"

export module engine.animation:ik;

import foundation.core;
import foundation.profiler;
import foundation.scene;
import foundation.render;
import foundation.animation;
import :components;

using namespace foundation::core;

export namespace engine::animation
{
    namespace scene = foundation::scene;
    namespace animation = foundation::animation;

    /// Where a component stands: solving, waiting for its animator's first tick, or disabled by
    /// what it cannot resolve (each disabled state logs once when it begins).
    enum class IkStatus : u8
    {
        Waiting,
        Solving,
        NoAnimator,
        UnknownBone,
        NotAChain,
    };

    /// One component's solve, run by the animator's player as a pose modifier. The manager fills it
    /// each frame (it holds its own copy: a component moves in its manager's storage, this does
    /// not); the player runs it after the pose and writes back the result and, for debug drawing,
    /// the chain in world space.
    class IkModifier final : public animation::IPoseModifier
    {
    public:
        enum class Kind : u8
        {
            TwoBone,
            Aim,
            Foot,
        };

        void Apply(const animation::Skeleton& skeleton, Span<animation::BoneTransform> local,
                   animation::ModelPoseCache& model) override
        {
            if (kind == Kind::Foot)
            {
                ApplyFoot(skeleton, local, model);
                return;
            }
            if (kind == Kind::TwoBone)
            {
                result = animation::SolveTwoBone(skeleton, local, model, chain, twoBone);
                drawn[0] = chain.start;
                drawn[1] = chain.mid;
                drawn[2] = chain.end;
                drawnCount = 3;
            }
            else
            {
                if (aimUpIsPoint && !aimBones.IsEmpty())
                {
                    const Float4x4& last = model.At(aimBones[aimBones.Size() - 1]);
                    aim.up = aimUpPoint - Float3{last.m[3][0], last.m[3][1], last.m[3][2]};
                }
                result = animation::SolveAim(skeleton, local, model,
                                             Span<const i32>{aimBones.Data(), aimBones.Size()},
                                             Span<const f32>{aimShares.Data(), aimShares.Size()}, aim);
                drawnCount = Min(aimBones.Size(), animation::kMaxAimBones);
                for (usize i = 0; i < drawnCount; ++i)
                {
                    drawn[i] = aimBones[i];
                }
            }
            DrawnToWorld(model);
            solved = true;
        }

        void DrawnToWorld(const animation::ModelPoseCache& model)
        {
            for (usize i = 0; i < drawnCount; ++i)
            {
                const Float4x4& m = model.At(drawn[i]);
                chainWorld[i] = TransformPoint(Float3{m.m[3][0], m.m[3][1], m.m[3][2]}, modelToWorld);
            }
        }

        // Feet: probe the ground under the animated feet once a frame (a second evaluation in the
        // same frame reuses the hits and holds the easing: no extra rays, no double step), then
        // solve.
        void ApplyFoot(const animation::Skeleton& skeleton, Span<animation::BoneTransform> local,
                       animation::ModelPoseCache& model)
        {
            const usize legs = Min(footLegs.Size(), animation::kMaxFootIkLegs);
            f32 step = 0.0f;
            if (footNewFrame)
            {
                footNewFrame = false;
                step = footStep;
                Float3 feet[animation::kMaxFootIkLegs];
                animation::FootIkAnimatedFeet(model, Span<const animation::FootIkLeg>{footLegs.Data(), legs},
                                              Span<Float3>{feet, legs});
                const Float3 up = Normalized(foot.up);
                const Float3 downWorld = TransformDirection(-up, modelToWorld);
                const f32 scale = Length(downWorld); // model to world length
                for (usize i = 0; i < legs; ++i)
                {
                    footGrounds[i] = animation::FootGround{};
                    footHitWorld[i] = false;
                    scene::SceneRayHit hit;
                    if (rays == nullptr || scale <= kEpsilon)
                    {
                        continue;
                    }
                    const Float3 origin = TransformPoint(feet[i] + up * footRayUp, modelToWorld);
                    if (rays->CastRay(origin, downWorld * (1.0f / scale), (footRayUp + footRayDown) * scale,
                                      footGroupMask, hit))
                    {
                        footGrounds[i].hit = true;
                        footGrounds[i].point = TransformPoint(hit.position, worldToModel);
                        footGrounds[i].normal = Normalized(TransformDirection(hit.normal, worldToModel));
                        footHitWorld[i] = true;
                        footGroundWorld[i] = hit.position;
                    }
                }
            }
            footResult = animation::SolveFootIk(skeleton, local, model, footPelvis,
                                                Span<const animation::FootIkLeg>{footLegs.Data(), legs},
                                                Span<const animation::FootGround>{footGrounds, legs}, foot,
                                                footState, step);
            result.valid = footResult.valid;
            result.error = 0.0f;
            for (usize i = 0; i < legs; ++i)
            {
                result.error = Max(result.error, footResult.footError[i]);
            }
            result.reached = result.valid && result.error <= kFootReachTolerance;
            drawnCount = 0;
            for (usize i = 0; i < legs; ++i)
            {
                drawn[drawnCount++] = footLegs[i].chain.start;
                drawn[drawnCount++] = footLegs[i].chain.mid;
                drawn[drawnCount++] = footLegs[i].chain.end;
            }
            drawnStride = 3;
            DrawnToWorld(model);
            solved = true;
        }

        /// A planted foot within this of its ground counts as reached (metres).
        static constexpr f32 kFootReachTolerance = 1.0e-3f;

        Kind kind = Kind::TwoBone;
        animation::TwoBoneIkChain chain;
        animation::TwoBoneIkSettings twoBone;
        Array<i32> aimBones;
        Array<f32> aimShares;
        animation::AimIkSettings aim;
        bool aimUpIsPoint = false;  // aim.up is computed from this point and the last bone
        Float3 aimUpPoint{};        // model space
        Float4x4 modelToWorld = Float4x4::Identity();
        Float4x4 worldToModel = Float4x4::Identity();

        // Feet (Kind::Foot).
        Array<animation::FootIkLeg> footLegs;
        i32 footPelvis = -1;
        animation::FootIkSettings foot;
        animation::FootIkState footState;
        animation::FootIkResult footResult;
        animation::FootGround footGrounds[animation::kMaxFootIkLegs];
        bool footHitWorld[animation::kMaxFootIkLegs] = {};
        Float3 footGroundWorld[animation::kMaxFootIkLegs] = {};
        f32 footRayUp = 0.5f;
        f32 footRayDown = 0.75f;
        u32 footGroupMask = 0xFFFFFFFFu;
        f32 footStep = 0.0f;        // this frame's seconds, handed over once
        bool footNewFrame = false;  // the next evaluation probes and steps
        scene::ISceneRayQuery* rays = nullptr; // the scene's solid-surface rays (null: no ground)

        animation::IkResult result; // the last solve
        bool solved = false;
        i32 drawn[animation::kMaxAimBones] = {};
        Float3 chainWorld[animation::kMaxAimBones] = {};
        usize drawnCount = 0;
        usize drawnStride = 0; // points per polyline (0: one polyline through them all)
    };

    /// A component's runtime side (none of it is saved).
    struct IkRuntime
    {
        UniquePtr<IkModifier> modifier;
        scene::EntityHandle animator = scene::EntityHandle::Invalid();
        const animation::Skeleton* resolvedFor = nullptr; // the skeleton the chain was resolved on
        IkStatus status = IkStatus::Waiting;
        IkStatus logged = IkStatus::Waiting;
        f32 weight = 0.0f; // eased toward the component's weight (or 0 when inactive)
        i32 order = 0;     // the order it was added to the stack at
        bool hasScriptTarget = false;
        Float3 scriptTarget{}; // world space (SceneAnimation.setIkTarget)
        Float3 targetWorld{};
        bool hasPoleWorld = false;
        Float3 poleWorld{};
    };

    /// A two-bone chain bent so its end reaches a target: a foot on a step, a hand on a handle.
    struct TwoBoneIkComponent
    {
        String startBone; // the chain by bone names (thigh, shin, foot)
        String midBone;
        String endBone;
        scene::EntityRef target; // empty: setIkTarget's point, else this component's own entity
        bool matchRotation = false; // the end bone takes the target's rotation as well
        scene::EntityRef pole;      // optional: the mid joint bends toward it
        Float3 hingeAxis{};         // for a straight chain with no pole and a straight bind pose
        f32 weight = 1.0f;
        f32 fadeSeconds = 0.2f; // the weight eases on and off over this
        bool active = true;
        i32 order = 0;          // lower first, across every IK component of one animator
        bool debugDraw = false; // draw the target and the chain while the scene runs
        IkRuntime runtime;
    };

    inline void Serialize(ISerializer& ar, TwoBoneIkComponent& c)
    {
        foundation::core::Serialize(ar, "startBone", c.startBone);
        foundation::core::Serialize(ar, "midBone", c.midBone);
        foundation::core::Serialize(ar, "endBone", c.endBone);
        foundation::core::Serialize(ar, "target", c.target);
        foundation::core::Serialize(ar, "matchRotation", c.matchRotation);
        foundation::core::Serialize(ar, "pole", c.pole);
        foundation::core::Serialize(ar, "hingeAxis", c.hingeAxis);
        foundation::core::Serialize(ar, "weight", c.weight);
        foundation::core::Serialize(ar, "fadeSeconds", c.fadeSeconds);
        foundation::core::Serialize(ar, "active", c.active);
        foundation::core::Serialize(ar, "order", c.order);
        foundation::core::Serialize(ar, "debugDraw", c.debugDraw);
    }

    /// One bone of an aim and its share of the swing still to go.
    struct AimIkBone
    {
        String bone;
        f32 share = 1.0f;
    };

    inline void Serialize(ISerializer& ar, AimIkBone& b)
    {
        foundation::core::Serialize(ar, "bone", b.bone);
        foundation::core::Serialize(ar, "share", b.share);
    }

    /// Bones that turn so the last one points at a target: a head that follows, a spine sharing the
    /// turn (0.3, 0.5, 1.0), a weapon.
    struct AimIkComponent
    {
        Array<AimIkBone> bones;  // root first; the last one aims
        scene::EntityRef target; // empty: setIkTarget's point, else this component's own entity
        scene::EntityRef up;     // optional: the up axis leans toward it (else the animated up)
        Float3 aimAxis{0.0f, 0.0f, 1.0f}; // in the last bone's space
        Float3 upAxis{0.0f, 1.0f, 0.0f};
        f32 maxAngle = 60.0f; // degrees from the animated direction
        f32 weight = 1.0f;
        f32 fadeSeconds = 0.2f;
        bool active = true;
        i32 order = 0;
        bool debugDraw = false;
        IkRuntime runtime;
    };

    inline void Serialize(ISerializer& ar, AimIkComponent& c)
    {
        foundation::core::Serialize(ar, "bones", c.bones);
        foundation::core::Serialize(ar, "target", c.target);
        foundation::core::Serialize(ar, "up", c.up);
        foundation::core::Serialize(ar, "aimAxis", c.aimAxis);
        foundation::core::Serialize(ar, "upAxis", c.upAxis);
        foundation::core::Serialize(ar, "maxAngle", c.maxAngle);
        foundation::core::Serialize(ar, "weight", c.weight);
        foundation::core::Serialize(ar, "fadeSeconds", c.fadeSeconds);
        foundation::core::Serialize(ar, "active", c.active);
        foundation::core::Serialize(ar, "order", c.order);
        foundation::core::Serialize(ar, "debugDraw", c.debugDraw);
    }

    /// One leg of a FootIkComponent: its chain by bone names (the end bone is the foot).
    struct FootIkLegBones
    {
        String startBone;
        String midBone;
        String endBone;
        Float3 hingeAxis{}; // for a straight leg with a straight bind pose (the knee's axis)
    };

    inline void Serialize(ISerializer& ar, FootIkLegBones& l)
    {
        foundation::core::Serialize(ar, "startBone", l.startBone);
        foundation::core::Serialize(ar, "midBone", l.midBone);
        foundation::core::Serialize(ar, "endBone", l.endBone);
        foundation::core::Serialize(ar, "hingeAxis", l.hingeAxis);
    }

    /// Feet that stand on the ground under them: each planted foot finds its ground with a ray
    /// (through the scene's ray query: physics, solid surfaces only), rises or falls to it and turns
    /// to its slope; the pelvis lowers so the lower foot reaches; a foot the animation lifts is
    /// swinging and left alone.
    struct FootIkComponent
    {
        Array<FootIkLegBones> legs; // up to four
        String pelvisBone;          // lowers to let the lower foot reach (empty: it stays)
        f32 rayUp = 0.5f;           // the probe starts this far above the animated foot
        f32 rayDown = 0.75f;        // and looks this far below it
        u32 groupMask = 0xFFFFFFFFu; // the collision groups that are ground
        f32 pelvisDropMax = 0.3f;
        f32 maxTilt = 30.0f;     // degrees a foot turns to its ground's slope
        f32 liftHeight = 0.15f;  // a foot animated higher than this is swinging
        f32 groundHeight = 0.0f; // the animation's ground along up (0: the model's origin is at its feet)
        f32 raiseRate = 20.0f;   // how fast a correction eases while it rises, per second
        f32 lowerRate = 8.0f;    // and while it lowers
        f32 weight = 1.0f;
        f32 fadeSeconds = 0.2f;
        bool active = true;
        i32 order = 0;
        bool debugDraw = false;
        IkRuntime runtime;
    };

    inline void Serialize(ISerializer& ar, FootIkComponent& c)
    {
        foundation::core::Serialize(ar, "legs", c.legs);
        foundation::core::Serialize(ar, "pelvisBone", c.pelvisBone);
        foundation::core::Serialize(ar, "rayUp", c.rayUp);
        foundation::core::Serialize(ar, "rayDown", c.rayDown);
        foundation::core::Serialize(ar, "groupMask", c.groupMask);
        foundation::core::Serialize(ar, "pelvisDropMax", c.pelvisDropMax);
        foundation::core::Serialize(ar, "maxTilt", c.maxTilt);
        foundation::core::Serialize(ar, "liftHeight", c.liftHeight);
        foundation::core::Serialize(ar, "groundHeight", c.groundHeight);
        foundation::core::Serialize(ar, "raiseRate", c.raiseRate);
        foundation::core::Serialize(ar, "lowerRate", c.lowerRate);
        foundation::core::Serialize(ar, "weight", c.weight);
        foundation::core::Serialize(ar, "fadeSeconds", c.fadeSeconds);
        foundation::core::Serialize(ar, "active", c.active);
        foundation::core::Serialize(ar, "order", c.order);
        foundation::core::Serialize(ar, "debugDraw", c.debugDraw);
    }

    /// The animator an IK component drives, as found this frame.
    struct IkAnimatorLink
    {
        scene::EntityHandle entity = scene::EntityHandle::Invalid();
        animation::PoseModifierStack* stack = nullptr;    // null until the player is built
        const animation::Skeleton* skeleton = nullptr;
        scene::EntityHandle modelEntity = scene::EntityHandle::Invalid(); // whose world is model space
    };

    namespace ik_detail
    {
        [[nodiscard]] inline scene::EntityHandle ModelEntity(scene::Scene& scene,
                                                             Span<const scene::EntityRef> meshes,
                                                             scene::EntityHandle animator)
        {
            for (const scene::EntityRef& r : meshes)
            {
                const scene::EntityHandle e = scene.FindEntity(r.id);
                if (scene.IsValid(e))
                {
                    return e;
                }
            }
            return animator;
        }

        /// The animator on `entity` itself, if any (a graph before a single clip, as they tick).
        [[nodiscard]] inline bool AnimatorOn(scene::Scene& scene, scene::EntityHandle entity,
                                             IkAnimatorLink& out)
        {
            if (auto* graphs = scene.GetSystem<AnimationGraphComponentManager>())
            {
                if (AnimationGraphComponent* g = graphs->Get(entity))
                {
                    out.entity = entity;
                    out.stack = g->player.Get() != nullptr ? &g->player->Modifiers() : nullptr;
                    out.skeleton = g->playerSkeleton;
                    out.modelEntity = ModelEntity(
                        scene, Span<const scene::EntityRef>{g->meshEntities.Data(), g->meshEntities.Size()},
                        entity);
                    return true;
                }
            }
            if (auto* clips = scene.GetSystem<SkeletalAnimationComponentManager>())
            {
                if (SkeletalAnimationComponent* s = clips->Get(entity))
                {
                    out.entity = entity;
                    out.stack = s->player.Get() != nullptr ? &s->player->Modifiers() : nullptr;
                    out.skeleton = s->playerSkeleton;
                    out.modelEntity = ModelEntity(
                        scene, Span<const scene::EntityRef>{s->meshEntities.Data(), s->meshEntities.Size()},
                        entity);
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] inline Float3 Translation(const Float4x4& m) noexcept
        {
            return Float3{m.m[3][0], m.m[3][1], m.m[3][2]};
        }
    }

    /// What authoring needs of the animator above an IK component, from the component data alone
    /// (an edit scene has no players): its skeleton, once the resource is loaded, and the entity
    /// whose world is the skeleton's model space.
    struct IkAuthoringAnimator
    {
        const animation::Skeleton* skeleton = nullptr;
        scene::EntityHandle modelEntity = scene::EntityHandle::Invalid();
    };

    /// The nearest animator at or above `from`, for the editor (bone pickers, gizmos). False when
    /// there is none; `skeleton` stays null until the animator's skeleton has loaded.
    [[nodiscard]] inline bool FindIkAuthoringAnimator(scene::Scene& scene, scene::EntityHandle from,
                                                      IkAuthoringAnimator& out)
    {
        scene::EntityHandle e = from;
        auto* graphs = scene.GetSystem<AnimationGraphComponentManager>();
        auto* clips = scene.GetSystem<SkeletalAnimationComponentManager>();
        for (u32 depth = 0; scene.IsValid(e) && depth < 1024u; ++depth)
        {
            if (AnimationGraphComponent* g = graphs != nullptr ? graphs->Get(e) : nullptr)
            {
                out.skeleton = g->skeleton.Get();
                out.modelEntity = ik_detail::ModelEntity(
                    scene, Span<const scene::EntityRef>{g->meshEntities.Data(), g->meshEntities.Size()}, e);
                return true;
            }
            if (SkeletalAnimationComponent* s = clips != nullptr ? clips->Get(e) : nullptr)
            {
                out.skeleton = s->skeleton.Get();
                out.modelEntity = ik_detail::ModelEntity(
                    scene, Span<const scene::EntityRef>{s->meshEntities.Data(), s->meshEntities.Size()}, e);
                return true;
            }
            e = scene.GetParent(e);
        }
        return false;
    }

    /// A bone's bind-pose position in the world, through the model entity (the editor's gizmos
    /// draw the chain where it stands before any animation). False for a name not in the skeleton.
    [[nodiscard]] inline bool IkBindBoneWorld(scene::Scene& scene, const IkAuthoringAnimator& animator,
                                              StringView bone, Float3& out)
    {
        if (animator.skeleton == nullptr)
        {
            return false;
        }
        const i32 index = animator.skeleton->FindBone(bone);
        const animation::Bone* b = index >= 0 ? animator.skeleton->GetBone(index) : nullptr;
        if (b == nullptr)
        {
            return false;
        }
        const Float4x4 model = Inverse(b->inverseBindPose);
        out = TransformPoint(Float3{model.m[3][0], model.m[3][1], model.m[3][2]},
                             scene.ComposeWorldMatrix(animator.modelEntity));
        return true;
    }

    /// The nearest animator at or above `from`.
    [[nodiscard]] inline bool FindIkAnimator(scene::Scene& scene, scene::EntityHandle from, IkAnimatorLink& out)
    {
        scene::EntityHandle e = from;
        for (u32 depth = 0; scene.IsValid(e) && depth < 1024u; ++depth)
        {
            if (ik_detail::AnimatorOn(scene, e, out))
            {
                return true;
            }
            e = scene.GetParent(e);
        }
        return false;
    }

    /// Takes a component's modifier off its animator's player, if both are still there.
    inline void DetachIk(scene::Scene& scene, IkRuntime& runtime)
    {
        if (runtime.modifier.Get() == nullptr || !scene.IsValid(runtime.animator))
        {
            return;
        }
        IkAnimatorLink link;
        if (ik_detail::AnimatorOn(scene, runtime.animator, link) && link.stack != nullptr)
        {
            link.stack->Remove(runtime.modifier.Get());
        }
    }

    /// Draws a component's last solve: the chain, its target (green reached, orange not), its pole.
    inline void DrawIk(foundation::render::debug::DebugDraw& draw, const IkRuntime& runtime)
    {
        const IkModifier* m = runtime.modifier.Get();
        if (m == nullptr || !m->solved || runtime.status != IkStatus::Solving)
        {
            return;
        }
        const Color chain{0.3f, 0.7f, 1.0f, 1.0f};
        const Color reached{0.2f, 0.9f, 0.3f, 1.0f};
        const Color missed{1.0f, 0.55f, 0.1f, 1.0f};
        for (usize i = 0; i + 1 < m->drawnCount; ++i)
        {
            if (m->drawnStride == 0 || (i + 1) % m->drawnStride != 0)
            {
                draw.DrawLine(m->chainWorld[i], m->chainWorld[i + 1], chain, true);
            }
        }
        for (usize i = 0; i < m->drawnCount; ++i)
        {
            draw.DrawWireSphere(m->chainWorld[i], 0.02f, chain, 8, true);
        }
        if (m->kind == IkModifier::Kind::Foot)
        {
            // Each leg's ground: green where a planted foot found it, orange where none was found.
            for (usize i = 0; i < m->footLegs.Size() && i < animation::kMaxFootIkLegs; ++i)
            {
                const Float3 foot = m->chainWorld[i * 3 + 2];
                if (m->footHitWorld[i])
                {
                    draw.DrawLine(foot, m->footGroundWorld[i], reached, true);
                    draw.DrawWireSphere(m->footGroundWorld[i], 0.04f, reached, 10, true);
                }
                else
                {
                    draw.DrawWireSphere(foot, 0.04f, missed, 10, true);
                }
            }
            return;
        }
        draw.DrawWireSphere(runtime.targetWorld, 0.05f, m->result.reached ? reached : missed, 12, true);
        if (runtime.hasPoleWorld && m->drawnCount > 1)
        {
            draw.DrawLine(m->chainWorld[1], runtime.poleWorld, Color{0.8f, 0.4f, 1.0f, 1.0f}, true);
        }
    }

    /// The work every IK manager shares: the animator, the chain, the fade, the stack. `Derived`
    /// supplies Resolve (bone names to indices; false names the bone that failed) and Fill (the
    /// frame's targets in model space).
    template <typename T, typename Derived>
    class IkComponentManagerBase : public scene::SerializableComponentManager<T>
    {
    public:
        explicit IkComponentManagerBase(StringView typeId) : scene::SerializableComponentManager<T>(typeId) {}

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }
        // Before the animation graph (-1) and single-clip (0) managers, whose players run the solve.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return -2; }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            PROFILE_SCOPE("Animation.Ik");
            this->ForEach([&](T& c, scene::EntityHandle owner) { Step(c, owner, deltaTime); });
        }

        /// Draws every component asking for it (debugDraw) into `draw`.
        void DrawDebug(foundation::render::debug::DebugDraw& draw)
        {
            this->ForEach(
                [&](T& c, scene::EntityHandle)
                {
                    if (c.debugDraw)
                    {
                        DrawIk(draw, c.runtime);
                    }
                });
        }

    protected:
        void OnComponentDestroyed(T& c, scene::EntityHandle) override
        {
            if (m_scene != nullptr)
            {
                DetachIk(*m_scene, c.runtime);
            }
        }

        scene::Scene* m_scene = nullptr;

    private:
        void Disable(T& c, scene::EntityHandle owner, IkStatus status, StringView detail)
        {
            DetachIk(*m_scene, c.runtime);
            c.runtime.status = status;
            c.runtime.weight = 0.0f;
            if (c.runtime.logged != status)
            {
                c.runtime.logged = status;
                LOG_WARNING(u8"Animation", u8"inverse kinematics on '{}' is off: {}",
                            m_scene->GetEntityName(owner), detail);
            }
        }

        void Step(T& c, scene::EntityHandle owner, f32 deltaTime)
        {
            IkRuntime& rt = c.runtime;
            if (rt.modifier.Get() == nullptr)
            {
                rt.modifier = MakeUnique<IkModifier>(m_scene->Allocator());
            }
            if (!m_scene->IsEffectivelyActive(owner))
            {
                DetachIk(*m_scene, rt);
                rt.weight = 0.0f;
                return;
            }
            IkAnimatorLink link;
            if (!FindIkAnimator(*m_scene, owner, link))
            {
                Disable(c, owner, IkStatus::NoAnimator, u8"no animation graph or skeletal animation at or above it");
                return;
            }
            if (link.entity != rt.animator)
            {
                DetachIk(*m_scene, rt);
                rt.animator = link.entity;
                rt.resolvedFor = nullptr;
            }
            if (link.stack == nullptr || link.skeleton == nullptr)
            {
                rt.status = IkStatus::Waiting; // the animator builds its player on its first tick
                return;
            }
            if (rt.resolvedFor != link.skeleton)
            {
                String failed;
                const IkStatus resolved = static_cast<Derived*>(this)->Resolve(c, *link.skeleton, failed);
                if (resolved != IkStatus::Solving)
                {
                    rt.resolvedFor = nullptr;
                    Disable(c, owner, resolved, failed.AsView());
                    return;
                }
                rt.resolvedFor = link.skeleton;
            }
            rt.status = IkStatus::Solving;
            rt.logged = IkStatus::Solving;

            // The weight eases toward its goal at full scale per fadeSeconds.
            const f32 goal = c.active ? Clamp(c.weight, 0.0f, 1.0f) : 0.0f;
            const f32 step = c.fadeSeconds > 0.0f ? deltaTime / c.fadeSeconds : 1.0f;
            rt.weight = rt.weight < goal ? Min(goal, rt.weight + step) : Max(goal, rt.weight - step);
            if (rt.weight <= 0.0f)
            {
                DetachIk(*m_scene, rt);
                return;
            }

            const Float4x4 modelToWorld = m_scene->ComposeWorldMatrix(link.modelEntity);
            const Float4x4 worldToModel = Inverse(modelToWorld);
            rt.modifier->modelToWorld = modelToWorld;
            rt.modifier->worldToModel = worldToModel;
            if (!static_cast<Derived*>(this)->Fill(c, owner, worldToModel, deltaTime))
            {
                DetachIk(*m_scene, rt); // the target entity is gone: nothing to reach this frame
                return;
            }
            if (rt.order != c.order)
            {
                link.stack->Remove(rt.modifier.Get());
                rt.order = c.order;
            }
            link.stack->Add(rt.modifier.Get(), rt.order); // no-op while it is there
        }
    };

    namespace ik_detail
    {
        /// The world point a component reaches for: its target entity, else the script's point,
        /// else its own entity. False when a named target entity is gone.
        [[nodiscard]] inline bool TargetWorld(scene::Scene& scene, const scene::EntityRef& target,
                                              const IkRuntime& rt, scene::EntityHandle owner,
                                              Float4x4& outWorld)
        {
            if (!target.IsNil())
            {
                const scene::EntityHandle e = scene.FindEntity(target.id);
                if (!scene.IsValid(e))
                {
                    return false;
                }
                outWorld = scene.ComposeWorldMatrix(e);
                return true;
            }
            if (rt.hasScriptTarget)
            {
                outWorld = Float4x4::Translation(rt.scriptTarget);
                return true;
            }
            outWorld = scene.ComposeWorldMatrix(owner);
            return true;
        }
    }

    class TwoBoneIkComponentManager final
        : public IkComponentManagerBase<TwoBoneIkComponent, TwoBoneIkComponentManager>
    {
    public:
        TwoBoneIkComponentManager() : IkComponentManagerBase(u8"two_bone_ik") {}

        [[nodiscard]] IkStatus Resolve(TwoBoneIkComponent& c, const animation::Skeleton& skeleton, String& failed)
        {
            const String* names[3] = {&c.startBone, &c.midBone, &c.endBone};
            i32 bones[3] = {-1, -1, -1};
            for (usize i = 0; i < 3; ++i)
            {
                bones[i] = skeleton.FindBone(names[i]->AsView());
                if (bones[i] < 0)
                {
                    failed = Format(u8"no bone named '{}' in its animator's skeleton", names[i]->AsView());
                    return IkStatus::UnknownBone;
                }
            }
            if (!animation::ik::IsBelow(skeleton, bones[1], bones[0]) ||
                !animation::ik::IsBelow(skeleton, bones[2], bones[1]))
            {
                failed = Format(u8"'{}', '{}', '{}' are not a chain (each below the one before)",
                                c.startBone.AsView(), c.midBone.AsView(), c.endBone.AsView());
                return IkStatus::NotAChain;
            }
            IkModifier& m = *c.runtime.modifier;
            m.kind = IkModifier::Kind::TwoBone;
            m.chain = animation::TwoBoneIkChain{bones[0], bones[1], bones[2]};
            return IkStatus::Solving;
        }

        [[nodiscard]] bool Fill(TwoBoneIkComponent& c, scene::EntityHandle owner, const Float4x4& worldToModel, f32)
        {
            IkRuntime& rt = c.runtime;
            Float4x4 targetWorld;
            if (!ik_detail::TargetWorld(*m_scene, c.target, rt, owner, targetWorld))
            {
                return false;
            }
            animation::TwoBoneIkSettings& s = rt.modifier->twoBone;
            rt.targetWorld = ik_detail::Translation(targetWorld);
            s.target = TransformPoint(rt.targetWorld, worldToModel);
            s.matchRotation = c.matchRotation;
            if (c.matchRotation)
            {
                s.targetRotation = animation::ik::RotationOf(targetWorld * worldToModel);
            }
            s.hasPole = false;
            rt.hasPoleWorld = false;
            if (!c.pole.IsNil())
            {
                const scene::EntityHandle pole = m_scene->FindEntity(c.pole.id);
                if (m_scene->IsValid(pole))
                {
                    rt.poleWorld = ik_detail::Translation(m_scene->ComposeWorldMatrix(pole));
                    rt.hasPoleWorld = true;
                    s.hasPole = true;
                    s.pole = TransformPoint(rt.poleWorld, worldToModel);
                }
            }
            s.hingeAxis = c.hingeAxis;
            s.weight = rt.weight;
            return true;
        }
    };

    class AimIkComponentManager final : public IkComponentManagerBase<AimIkComponent, AimIkComponentManager>
    {
    public:
        AimIkComponentManager() : IkComponentManagerBase(u8"aim_ik") {}

        [[nodiscard]] IkStatus Resolve(AimIkComponent& c, const animation::Skeleton& skeleton, String& failed)
        {
            if (c.bones.IsEmpty() || c.bones.Size() > animation::kMaxAimBones)
            {
                failed = Format(u8"an aim takes 1 to {} bones, it lists {}", animation::kMaxAimBones,
                                c.bones.Size());
                return IkStatus::NotAChain;
            }
            IkModifier& m = *c.runtime.modifier;
            m.kind = IkModifier::Kind::Aim;
            m.aimBones.Clear();
            m.aimShares.Clear();
            for (usize i = 0; i < c.bones.Size(); ++i)
            {
                const i32 bone = skeleton.FindBone(c.bones[i].bone.AsView());
                if (bone < 0)
                {
                    failed = Format(u8"no bone named '{}' in its animator's skeleton", c.bones[i].bone.AsView());
                    return IkStatus::UnknownBone;
                }
                if (i > 0 && !animation::ik::IsBelow(skeleton, bone, m.aimBones[i - 1]))
                {
                    failed = Format(u8"'{}' is not below '{}'", c.bones[i].bone.AsView(),
                                    c.bones[i - 1].bone.AsView());
                    return IkStatus::NotAChain;
                }
                m.aimBones.PushBack(bone);
                m.aimShares.PushBack(c.bones[i].share);
            }
            return IkStatus::Solving;
        }

        [[nodiscard]] bool Fill(AimIkComponent& c, scene::EntityHandle owner, const Float4x4& worldToModel, f32)
        {
            IkRuntime& rt = c.runtime;
            Float4x4 targetWorld;
            if (!ik_detail::TargetWorld(*m_scene, c.target, rt, owner, targetWorld))
            {
                return false;
            }
            IkModifier& m = *rt.modifier;
            // The shares follow the component's (an inspector edit needs no re-resolve).
            for (usize i = 0; i < m.aimShares.Size() && i < c.bones.Size(); ++i)
            {
                m.aimShares[i] = c.bones[i].share;
            }
            animation::AimIkSettings& s = m.aim;
            rt.targetWorld = ik_detail::Translation(targetWorld);
            s.target = TransformPoint(rt.targetWorld, worldToModel);
            s.aimAxis = c.aimAxis;
            s.upAxis = c.upAxis;
            s.maxAngle = c.maxAngle * kDegToRad;
            s.weight = rt.weight;
            s.hasUp = false;
            m.aimUpIsPoint = false;
            rt.hasPoleWorld = false;
            if (!c.up.IsNil())
            {
                const scene::EntityHandle up = m_scene->FindEntity(c.up.id);
                if (m_scene->IsValid(up))
                {
                    rt.poleWorld = ik_detail::Translation(m_scene->ComposeWorldMatrix(up));
                    rt.hasPoleWorld = true;
                    s.hasUp = true;
                    m.aimUpIsPoint = true;
                    m.aimUpPoint = TransformPoint(rt.poleWorld, worldToModel);
                }
            }
            return true;
        }
    };

    class FootIkComponentManager final : public IkComponentManagerBase<FootIkComponent, FootIkComponentManager>
    {
    public:
        FootIkComponentManager() : IkComponentManagerBase(u8"foot_ik") {}

        [[nodiscard]] IkStatus Resolve(FootIkComponent& c, const animation::Skeleton& skeleton, String& failed)
        {
            if (c.legs.IsEmpty() || c.legs.Size() > animation::kMaxFootIkLegs)
            {
                failed = Format(u8"foot IK takes 1 to {} legs, it lists {}", animation::kMaxFootIkLegs,
                                c.legs.Size());
                return IkStatus::NotAChain;
            }
            IkModifier& m = *c.runtime.modifier;
            m.kind = IkModifier::Kind::Foot;
            m.footLegs.Clear();
            for (const FootIkLegBones& leg : c.legs)
            {
                const String* names[3] = {&leg.startBone, &leg.midBone, &leg.endBone};
                i32 bones[3] = {-1, -1, -1};
                for (usize i = 0; i < 3; ++i)
                {
                    bones[i] = skeleton.FindBone(names[i]->AsView());
                    if (bones[i] < 0)
                    {
                        failed = Format(u8"no bone named '{}' in its animator's skeleton", names[i]->AsView());
                        return IkStatus::UnknownBone;
                    }
                }
                if (!animation::ik::IsBelow(skeleton, bones[1], bones[0]) ||
                    !animation::ik::IsBelow(skeleton, bones[2], bones[1]))
                {
                    failed = Format(u8"'{}', '{}', '{}' are not a chain (each below the one before)",
                                    leg.startBone.AsView(), leg.midBone.AsView(), leg.endBone.AsView());
                    return IkStatus::NotAChain;
                }
                m.footLegs.PushBack(animation::FootIkLeg{animation::TwoBoneIkChain{bones[0], bones[1], bones[2]},
                                                         leg.hingeAxis});
            }
            m.footPelvis = -1;
            if (!c.pelvisBone.IsEmpty())
            {
                m.footPelvis = skeleton.FindBone(c.pelvisBone.AsView());
                if (m.footPelvis < 0)
                {
                    failed = Format(u8"no bone named '{}' in its animator's skeleton", c.pelvisBone.AsView());
                    return IkStatus::UnknownBone;
                }
            }
            m.footState = animation::FootIkState{}; // a new chain starts from its goals
            return IkStatus::Solving;
        }

        [[nodiscard]] bool Fill(FootIkComponent& c, scene::EntityHandle, const Float4x4&, f32 deltaTime)
        {
            IkModifier& m = *c.runtime.modifier;
            for (usize i = 0; i < m.footLegs.Size() && i < c.legs.Size(); ++i)
            {
                m.footLegs[i].hingeAxis = c.legs[i].hingeAxis;
            }
            animation::FootIkSettings& s = m.foot;
            s.pelvisDropMax = c.pelvisDropMax;
            s.maxTilt = c.maxTilt * kDegToRad;
            s.liftHeight = c.liftHeight;
            s.groundHeight = c.groundHeight;
            s.raiseRate = c.raiseRate;
            s.lowerRate = c.lowerRate;
            s.weight = c.runtime.weight;
            m.footRayUp = Max(c.rayUp, 0.0f);
            m.footRayDown = Max(c.rayDown, 0.0f);
            m.footGroupMask = c.groupMask;
            m.footStep = deltaTime;
            m.footNewFrame = true;
            m.rays = nullptr;
            m_scene->ForEachSystem(
                [&](scene::SceneSystem& system)
                {
                    if (m.rays == nullptr)
                    {
                        m.rays = system.AsRayQuery();
                    }
                });
            return true;
        }
    };

    inline void AddIkSceneManagers(scene::Scene& scene)
    {
        scene.AddSystem<TwoBoneIkComponentManager>();
        scene.AddSystem<AimIkComponentManager>();
        scene.AddSystem<FootIkComponentManager>();
    }
}
