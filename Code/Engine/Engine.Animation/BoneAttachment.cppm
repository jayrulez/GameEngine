// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Animation - the `:attachment` partition.
///
/// BoneAttachmentComponent: an entity that follows one bone of the nearest animator (an animation
/// graph or a skeletal animation component), as a weapon in a hand or a hat on a head does. The
/// bones animate only as a skinning palette, so an entity under a bone's node stands where the
/// file put it while the skin moves; this moves it. Its manager runs in PostUpdate after the
/// animators, while the scene simulates (standing still, the animators hold the rest pose, which
/// is where the bone's node already is), and sets the entity's transform from the bone's pose in
/// the skeleton's model space, through the animator's model entity (the first mesh entity, as IK
/// uses it). Its children ride along, each at its own offset. The importer puts one on the
/// nearest bone node above each unskinned mesh of a rigged model (foundation::model::RigidMeshBone).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Profiler/Profiler.h"

export module engine.animation:attachment;

import foundation.core;
import foundation.profiler;
import foundation.scene;
import foundation.animation;
import :components;
import :ik;

using namespace foundation::core;

export namespace engine::animation
{
    namespace scene = foundation::scene;
    namespace animation = foundation::animation;

    /// Where an attachment stands: following, waiting for its animator's first pose, or off for
    /// what it cannot resolve (each off state logs once when it begins).
    enum class BoneAttachmentStatus : u8
    {
        Waiting,
        Following,
        NoAnimator,
        UnknownBone,
    };

    /// An attachment's runtime side (none of it is saved).
    struct BoneAttachmentRuntime
    {
        const animation::Skeleton* resolvedFor = nullptr; // the skeleton the bone was found in
        i32 boneIndex = -1;
        Float4x4 bindModel = Float4x4::Identity(); // the bone's bind pose in model space
        BoneAttachmentStatus status = BoneAttachmentStatus::Waiting;
        BoneAttachmentStatus logged = BoneAttachmentStatus::Waiting;
    };

    /// Follows one bone of the animator at or above this entity (else the one below it).
    struct BoneAttachmentComponent
    {
        String bone; // the bone by name
        BoneAttachmentRuntime runtime;
    };

    inline void Serialize(ISerializer& ar, BoneAttachmentComponent& c)
    {
        foundation::core::Serialize(ar, "bone", c.bone);
    }

    class BoneAttachmentComponentManager final : public scene::SerializableComponentManager<BoneAttachmentComponent>
    {
    public:
        BoneAttachmentComponentManager() : scene::SerializableComponentManager<BoneAttachmentComponent>(u8"bone_attachment") {}

        void OnSceneCreate(scene::Scene& scene) override { m_scene = &scene; }
        // Standing still, the animators hold the rest pose, which is where the bone's node stands
        // already; writing it there each frame would only fight the editor's own edits.
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }
        // After the animation graph (-1) and single-clip (0) managers, whose palettes it reads.
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return 1; }

        void OnUpdate(scene::ScenePhase phase, f32) override
        {
            if (phase != scene::ScenePhase::PostUpdate || m_scene == nullptr)
            {
                return;
            }
            PROFILE_SCOPE("Animation.BoneAttachment");
            // Shallowest first: an attachment under another composes its parent's world from the
            // transform this frame just set (a hand's, then a finger's).
            m_order.Clear();
            ForEach(
                [&](BoneAttachmentComponent& c, scene::EntityHandle owner)
                {
                    u32 depth = 0;
                    for (scene::EntityHandle e = m_scene->GetParent(owner); m_scene->IsValid(e) && depth < 1024u;
                         e = m_scene->GetParent(e))
                    {
                        ++depth;
                    }
                    m_order.PushBack(Pending{&c, owner, depth});
                });
            for (usize i = 1; i < m_order.Size(); ++i)
            {
                const Pending p = m_order[i];
                usize j = i;
                for (; j > 0 && m_order[j - 1].depth > p.depth; --j)
                {
                    m_order[j] = m_order[j - 1];
                }
                m_order[j] = p;
            }
            for (const Pending& p : m_order)
            {
                Follow(*p.component, p.owner);
            }
        }

    private:
        struct Pending
        {
            BoneAttachmentComponent* component;
            scene::EntityHandle owner;
            u32 depth;
        };

        /// The animator's skeleton, palette and model entity, once its player has posed.
        struct Animator
        {
            const animation::Skeleton* skeleton = nullptr;
            Span<const Float4x4> palette;
            scene::EntityHandle modelEntity = scene::EntityHandle::Invalid();
        };

        [[nodiscard]] bool AnimatorOn(scene::EntityHandle e, Animator& out)
        {
            if (auto* graphs = m_scene->GetSystem<AnimationGraphComponentManager>())
            {
                if (AnimationGraphComponent* g = graphs->Get(e))
                {
                    out.skeleton = g->playerSkeleton;
                    out.palette = g->player.Get() != nullptr ? g->player->GetSkinningMatrices() : Span<const Float4x4>{};
                    out.modelEntity = ik_detail::ModelEntity(
                        *m_scene, Span<const scene::EntityRef>{g->meshEntities.Data(), g->meshEntities.Size()}, e);
                    return true;
                }
            }
            if (auto* clips = m_scene->GetSystem<SkeletalAnimationComponentManager>())
            {
                if (SkeletalAnimationComponent* s = clips->Get(e))
                {
                    out.skeleton = s->playerSkeleton;
                    out.palette = s->player.Get() != nullptr ? s->player->GetSkinningMatrices() : Span<const Float4x4>{};
                    out.modelEntity = ik_detail::ModelEntity(
                        *m_scene, Span<const scene::EntityRef>{s->meshEntities.Data(), s->meshEntities.Size()}, e);
                    return true;
                }
            }
            return false;
        }

        void Off(BoneAttachmentComponent& c, scene::EntityHandle owner, BoneAttachmentStatus status, StringView detail)
        {
            c.runtime.status = status;
            if (c.runtime.logged != status)
            {
                c.runtime.logged = status;
                LOG_WARNING(u8"Animation", u8"bone attachment on '{}' is off: {}", m_scene->GetEntityName(owner), detail);
            }
        }

        void Follow(BoneAttachmentComponent& c, scene::EntityHandle owner)
        {
            Animator animator;
            const ik_detail::Lookup found = ik_detail::FindAnimatorFrom(
                *m_scene, owner, [&](scene::EntityHandle e) { return AnimatorOn(e, animator); });
            if (found != ik_detail::Lookup::Found)
            {
                Off(c, owner, BoneAttachmentStatus::NoAnimator,
                    found == ik_detail::Lookup::Ambiguous ? u8"several animators below it, none above"
                                                          : u8"no animator at, above or below it");
                return;
            }
            if (animator.skeleton == nullptr || animator.palette.IsEmpty())
            {
                c.runtime.status = BoneAttachmentStatus::Waiting; // the player poses on its first tick
                return;
            }
            if (c.runtime.resolvedFor != animator.skeleton)
            {
                c.runtime.resolvedFor = animator.skeleton;
                c.runtime.boneIndex = animator.skeleton->FindBone(c.bone.AsView());
                const animation::Bone* bone =
                    c.runtime.boneIndex >= 0 ? animator.skeleton->GetBone(c.runtime.boneIndex) : nullptr;
                c.runtime.bindModel = bone != nullptr ? Inverse(bone->inverseBindPose) : Float4x4::Identity();
            }
            if (c.runtime.boneIndex < 0 || static_cast<usize>(c.runtime.boneIndex) >= animator.palette.Size())
            {
                Off(c, owner, BoneAttachmentStatus::UnknownBone, u8"its bone is not in the animator's skeleton");
                return;
            }
            c.runtime.status = BoneAttachmentStatus::Following;
            c.runtime.logged = BoneAttachmentStatus::Following;

            // The palette is the inverse bind times the bone's model pose (row vectors), so the bind
            // pose undoes it; the model entity's world (composed fresh: the cached world matrices
            // update after PostUpdate) takes it to the world, the parent's inverse to the local.
            const Float4x4 boneModel = c.runtime.bindModel * animator.palette[static_cast<usize>(c.runtime.boneIndex)];
            const Float4x4 world = boneModel * m_scene->ComposeWorldMatrix(animator.modelEntity);
            const scene::EntityHandle parent = m_scene->GetParent(owner);
            const Float4x4 local = m_scene->IsValid(parent) ? world * Inverse(m_scene->ComposeWorldMatrix(parent)) : world;
            m_scene->SetLocalTransform(owner, Transform::FromMatrix(local));
        }

        scene::Scene* m_scene = nullptr;
        Array<Pending> m_order;
    };
}
