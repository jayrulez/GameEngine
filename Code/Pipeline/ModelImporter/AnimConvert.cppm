// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Pipeline::ModelImporter:anim_convert - Model IR skin/animation -> animation *Source.
///
/// Converts a model's skin (joints + inverse-bind matrices + the bone hierarchy) into a
/// SkeletonSource, and its animation channels into AnimationClipSources. The skeleton's
/// bone order IS the skin's JOINT order (so a skinned vertex's joint indices address the
/// skeleton directly), and parent links + animation target bones are remapped from model
/// bone indices into joint indices (NodeToBoneMapping).

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

    // model bone index -> skeleton joint index (the skin's joint order). -1 for bones not in the skin.
    [[nodiscard]] inline HashMap<i32, i32> BuildBoneToJoint(const model::ModelSkin& skin)
    {
        HashMap<i32, i32> map;
        const Span<const i32> joints = skin.joints();
        for (usize j = 0; j < joints.Size(); ++j)
        {
            map.InsertOrAssign(joints[j], static_cast<i32>(j));
        }
        return map;
    }

    // The model node the skeleton hangs from: the parent of the skin's first root joint (a joint
    // whose parent is not in the skin), -1 when that root has no parent, -2 for a skin with no
    // joints. A skin with several roots under different nodes takes the first root's (the
    // skeleton has one model space).
    [[nodiscard]] inline i32 SkeletonParentNode(const model::Model& model, const model::ModelSkin& skin,
                                                const HashMap<i32, i32>& boneToJoint)
    {
        const Span<model::ModelBone* const> bones = model.bones();
        for (const i32 joint : skin.joints())
        {
            if (joint < 0 || static_cast<usize>(joint) >= bones.Size())
            {
                continue;
            }
            const i32 parent = bones[static_cast<usize>(joint)]->parentIndex;
            if (parent < 0 || static_cast<usize>(parent) >= bones.Size())
            {
                return -1;
            }
            if (!boneToJoint.Contains(parent))
            {
                return parent;
            }
        }
        return -2;
    }

    // Build a SkeletonSource from a skin: one bone per joint (joint order), local bind TRS from the
    // model bone, inverse-bind from the skin, parent remapped into joint space.
    inline void SkeletonSourceFromModel(const model::Model& model, const model::ModelSkin& skin,
                                        const HashMap<i32, i32>& boneToJoint,
                                        animation::SkeletonSource& out)
    {
        out.name = String(u8"skeleton");
        const Span<const i32> joints = skin.joints();
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
