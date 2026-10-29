// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Pipeline::ModelImporter:anim_convert - Model IR skin/animation -> animation *Source.
///
/// Converts a model's skin (joints + inverse-bind matrices + the bone hierarchy) into a
/// SkeletonSource, and its animation channels into AnimationClipSources. The skeleton's joints
/// are the skin's, in its order (so a skinned vertex's joint indices address the skeleton
/// directly), then the bones the skin's joints hang from (SkeletonJoints: a helper bone with no
/// vertices of its own, whose animation is part of the pose); parent links + animation target
/// bones are remapped from model bone indices into joint indices.

module;
#include "Core/Prelude.h"

export module modelimporter:anim_convert;

import foundation.core;
import foundation.model;
import foundation.animation;
import foundation.animation.resource;

using namespace foundation::core;
namespace model = foundation::model;
namespace animation = foundation::animation;

export namespace pipeline
{
    // True if the model mesh carries skinning (Joints/Weights vertex elements).
    [[nodiscard]] inline bool IsSkinnedMesh(const model::ModelMesh& mesh) noexcept
    {
        for (const model::VertexElement& e : mesh.vertexElements())
        {
            if (e.semantic == model::VertexSemantic::Joints)
            {
                return true;
            }
        }
        return false;
    }

    // The model node the joints `joints` hang from: the parent of their first root (a joint whose
    // parent is not among them), -1 when that root has no parent, -2 with no joints. Several roots
    // under different nodes take the first root's (the skeleton has one model space).
    [[nodiscard]] inline i32 JointsParentNode(const model::Model& model, Span<const i32> joints)
    {
        const Span<model::ModelBone* const> bones = model.bones();
        const auto among = [&](i32 bone)
        {
            for (const i32 joint : joints)
                if (joint == bone)
                    return true;
            return false;
        };
        for (const i32 joint : joints)
        {
            if (joint < 0 || static_cast<usize>(joint) >= bones.Size())
                continue;
            const i32 parent = bones[static_cast<usize>(joint)]->parentIndex;
            if (parent < 0 || static_cast<usize>(parent) >= bones.Size())
                return -1;
            if (!among(parent))
                return parent;
        }
        return -2;
    }

    // The skeleton's joints, as model bone indices: the skin's, in its order (a skinned vertex's
    // joint indices address them), then each node the skeleton hangs from while the file makes it
    // a bone (ModelBone::isSkeletonBone). 3ds Max hangs a skeleton from a Dummy helper bone with no
    // vertices of its own, and animates it with the pose (a creature's bob): it is a joint, its
    // channels played. An empty node above (Blender's armature object, whose animation is the
    // character's travel) stays outside: the node the skeleton hangs from, its channels the clip's
    // model tracks. Then the node each unskinned mesh hangs under, with the nodes between it and
    // the skeleton, when its chain reaches the skeleton: 3ds Max's Biped animates a weapon's prop
    // node with the hand (the file does not make it a bone), and no vertex is bound to it, so
    // without it the clip's prop channels were dropped and the weapon's bone attachment rode the
    // pelvis. Each is appended after the skin's joints, so no vertex's joint index moves.
    [[nodiscard]] inline Array<i32> SkeletonJoints(const model::Model& model, const model::ModelSkin& skin)
    {
        Array<i32> joints;
        for (const i32 joint : skin.joints())
            joints.PushBack(joint);
        const Span<model::ModelBone* const> bones = model.bones();
        for (usize guard = 0; guard < bones.Size(); ++guard) // a malformed cycle cannot loop for ever
        {
            const i32 hang = JointsParentNode(model, joints.AsSpan());
            if (hang < 0 || static_cast<usize>(hang) >= bones.Size() || !bones[static_cast<usize>(hang)]->isSkeletonBone)
                break;
            joints.PushBack(hang);
        }

        const auto among = [&](i32 bone)
        {
            for (const i32 joint : joints)
                if (joint == bone)
                    return true;
            return false;
        };
        const auto valid = [&](i32 bone) { return bone >= 0 && static_cast<usize>(bone) < bones.Size(); };
        for (const model::ModelBone* node : bones)
        {
            const Span<model::ModelMesh* const> meshes = model.meshes();
            if (node->meshIndex < 0 || static_cast<usize>(node->meshIndex) >= meshes.Size() ||
                IsSkinnedMesh(*meshes[static_cast<usize>(node->meshIndex)]))
                continue; // no mesh, or a skinned one (the palette moves it; a skin with no weights does not)
            // The node it hangs under.
            const i32 carrier = node->parentIndex;
            if (!valid(carrier) || among(carrier))
                continue;
            // The nodes from it up to the skeleton, joined parents first; none when it hangs apart.
            Array<i32> chain;
            i32 up = carrier;
            for (usize guard = 0; valid(up) && !among(up) && guard <= bones.Size(); ++guard)
            {
                chain.PushBack(up);
                up = bones[static_cast<usize>(up)]->parentIndex;
            }
            if (!valid(up))
                continue;
            for (usize i = chain.Size(); i-- > 0;)
                joints.PushBack(chain[i]);
        }
        return joints;
    }

    // model bone index -> skeleton joint index (`joints`' order, SkeletonJoints). -1 for bones not in it.
    [[nodiscard]] inline HashMap<i32, i32> BuildBoneToJoint(Span<const i32> joints)
    {
        HashMap<i32, i32> map;
        for (usize j = 0; j < joints.Size(); ++j)
        {
            map.InsertOrAssign(joints[j], static_cast<i32>(j));
        }
        return map;
    }

    // The model node the skeleton hangs from: the node above its joints (SkeletonJoints), -1 the
    // scene root, -2 no joints. Its channels are the clip's model tracks.
    [[nodiscard]] inline i32 SkeletonParentNode(const model::Model& model, Span<const i32> joints)
    {
        return JointsParentNode(model, joints);
    }

    // Build a SkeletonSource from a skin's skeleton joints (SkeletonJoints): one bone per joint (their
    // order), local bind TRS from the model bone, inverse-bind from the skin (identity for a bone the
    // skin's joints hang from: no vertex is bound to it), parent remapped into joint space.
    inline void SkeletonSourceFromModel(const model::Model& model, const model::ModelSkin& skin,
                                        Span<const i32> joints, const HashMap<i32, i32>& boneToJoint,
                                        animation::SkeletonSource& out)
    {
        out.name = String(u8"skeleton");
        const Span<const Float4x4> ibms = skin.inverseBindMatrices();
        const Span<model::ModelBone* const> bones = model.bones();

        for (usize j = 0; j < joints.Size(); ++j)
        {
            const i32 boneIdx = joints[j];
            const model::ModelBone* b = (boneIdx >= 0 && static_cast<usize>(boneIdx) < bones.Size())
                                            ? bones[boneIdx]
                                            : nullptr;

            out.boneNames.PushBack(b != nullptr ? String(b->name()) : String{});
            i32 parentJoint = -1;
            if (b != nullptr && b->parentIndex >= 0)
            {
                const i32* p = boneToJoint.Find(b->parentIndex);
                if (p != nullptr)
                {
                    parentJoint = *p;
                }
            }
            out.parentIndices.PushBack(parentJoint);
            out.translations.PushBack(b != nullptr ? b->translation : Float3{0, 0, 0});
            out.rotations.PushBack(b != nullptr ? b->rotation : Quaternion::Identity);
            out.scales.PushBack(b != nullptr ? b->scale : Float3{1, 1, 1});
            out.inverseBindPoses.PushBack(j < ibms.Size() ? ibms[j] : Float4x4::Identity());
        }
    }

    // Build an AnimationClipSource from a model animation: each channel becomes a dense track keyed by
    // JOINT index. A channel on `modelNode` (the node the skeleton hangs from: Blender's armature
    // object) becomes a MODEL track, bone -1: the pose never plays it, root motion may take the
    // armature's travel from it (root-motion.md P0). Other channels on bones outside the skin, and
    // morph-weight channels, are skipped.
    inline void AnimationClipSourceFromModel(const model::ModelAnimation& animation,
                                             const HashMap<i32, i32>& boneToJoint, StringView name,
                                             animation::AnimationClipSource& out, i32 modelNode = -2)
    {
        out.name = String(name);
        out.duration = animation.duration;
        out.isLooping = true;

        for (const model::AnimationChannel* ch : animation.channels())
        {
            if (ch == nullptr)
            {
                continue;
            }
            const i32* pj = boneToJoint.Find(ch->targetBone);
            const i32 modelTrack = -1;
            if (pj == nullptr && (modelNode < 0 || ch->targetBone != modelNode))
            {
                continue; // channel targets a bone not in this skin
            }
            if (pj == nullptr)
            {
                pj = &modelTrack;
            }

            u8 kind = 0;
            switch (ch->path)
            {
            case model::AnimationPath::Translation:
                kind = static_cast<u8>(animation::AnimationClipSource::TrackKind::Position);
                break;
            case model::AnimationPath::Rotation:
                kind = static_cast<u8>(animation::AnimationClipSource::TrackKind::Rotation);
                break;
            case model::AnimationPath::Scale:
                kind = static_cast<u8>(animation::AnimationClipSource::TrackKind::Scale);
                break;
            default:
                continue; // Weights (morph) not supported
            }
            u8 interp = static_cast<u8>(animation::InterpolationMode::Linear);
            if (ch->interpolation == model::AnimationInterpolation::Step)
            {
                interp = static_cast<u8>(animation::InterpolationMode::Step);
            }
            else if (ch->interpolation == model::AnimationInterpolation::CubicSpline)
            {
                interp = static_cast<u8>(animation::InterpolationMode::CubicSpline);
            }

            const Span<const model::AnimationKeyframe> keys = ch->keyframes();
            out.trackBone.PushBack(*pj);
            out.trackKind.PushBack(kind);
            out.trackInterp.PushBack(interp);
            out.trackStart.PushBack(static_cast<u32>(out.keyTimes.Size()));
            out.trackCount.PushBack(static_cast<u32>(keys.Size()));
            for (const model::AnimationKeyframe& k : keys)
            {
                out.keyTimes.PushBack(k.time);
                out.keyValues.PushBack(
                    k.value); // xyz for pos/scale, xyzw for rotation (matches FillClip)
            }
        }
    }

} // namespace pipeline
