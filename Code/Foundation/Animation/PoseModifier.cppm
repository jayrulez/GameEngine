// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Animation - the `:modifier` partition.
///
/// The stage between a player's pose and its skinning palette (inverse-kinematics.md P0b): a pose
/// modifier changes the local pose after the clip or graph produced it and before the palette is
/// built, every evaluation, so its change survives the next sample. Modifiers run in their order
/// over one model-space pose (ModelPoseCache) built once per evaluation and kept current by each
/// modifier's partial rebuild, so N chains do not cost N full rebuilds. Inverse kinematics is the
/// first user; the stage knows nothing of it.

module;
#include "Core/Prelude.h"

export module foundation.animation:modifier;

import foundation.core;
import :skeleton;
import :pose;

using namespace foundation::core;

export namespace foundation::animation
{
    /// The model-space (skeleton-space) matrices of a pose: built in full once per evaluation, then
    /// rebuilt from a changed bone downward as modifiers change it.
    class ModelPoseCache
    {
    public:
        ModelPoseCache() = default;
        /// The owner's allocator for the matrices and the rebuild's marks (sized once per skeleton).
        explicit ModelPoseCache(IAllocator& allocator) noexcept : m_model(allocator), m_touched(allocator) {}

        /// Every bone's model-space matrix from the local pose.
        void Build(const Skeleton& skeleton, Span<const BoneTransform> localPoses)
        {
            m_model.Resize(static_cast<usize>(skeleton.BoneCount()));
            Span<Float4x4> out{m_model.Data(), m_model.Size()};
            const Span<const i32> order = skeleton.HierarchicalOrder();
            if (!order.IsEmpty())
            {
                for (i32 bone : order)
                {
                    skeleton.ComputeBoneWorldPose(bone, localPoses, out);
                }
            }
            else
            {
                for (i32 bone = 0; bone < skeleton.BoneCount(); ++bone)
                {
                    skeleton.ComputeBoneWorldPose(bone, localPoses, out);
                }
            }
            ++m_fullBuilds;
        }

        /// `bone` and everything below it, after its local transform changed. Bones above it and
        /// beside it keep their matrices.
        void RebuildFrom(const Skeleton& skeleton, Span<const BoneTransform> localPoses, i32 bone)
        {
            const usize count = static_cast<usize>(skeleton.BoneCount());
            if (bone < 0 || static_cast<usize>(bone) >= count || m_model.Size() != count)
            {
                return;
            }
            m_touched.Resize(count);
            for (usize i = 0; i < count; ++i)
            {
                m_touched[i] = 0;
            }
            m_touched[static_cast<usize>(bone)] = 1;
            Span<Float4x4> out{m_model.Data(), m_model.Size()};
            for (i32 index : skeleton.HierarchicalOrder())
            {
                const Bone* b = skeleton.GetBone(index);
                const bool below = b != nullptr && b->parentIndex >= 0 &&
                                   m_touched[static_cast<usize>(b->parentIndex)] != 0;
                if (index == bone || below)
                {
                    m_touched[static_cast<usize>(index)] = 1;
                    skeleton.ComputeBoneWorldPose(index, localPoses, out);
                }
            }
        }

        [[nodiscard]] Span<const Float4x4> Model() const noexcept { return {m_model.Data(), m_model.Size()}; }
        [[nodiscard]] const Float4x4& At(i32 bone) const { return m_model[static_cast<usize>(bone)]; }
        /// Full builds so far (each evaluation with modifiers builds once).
        [[nodiscard]] u32 FullBuilds() const noexcept { return m_fullBuilds; }

    private:
        Array<Float4x4> m_model;
        Array<u8> m_touched;
        u32 m_fullBuilds = 0;
    };

    /// A change to a pose between its sampling and its palette. `localPoses` is the pose to change
    /// (parent-relative); `model` holds its model-space matrices, current on entry, and the
    /// modifier keeps it current (RebuildFrom the highest bone it changed) for the next one.
    class IPoseModifier
    {
    public:
        virtual ~IPoseModifier() = default;
        virtual void Apply(const Skeleton& skeleton, Span<BoneTransform> localPoses,
                           ModelPoseCache& model) = 0;
    };

    /// A player's modifiers, BORROWED (whoever adds one removes it before it goes), run lowest
    /// order first; equal orders keep the order they were added in.
    class PoseModifierStack
    {
    public:
        void Add(IPoseModifier* modifier, i32 order = 0)
        {
            if (modifier == nullptr || Contains(modifier))
            {
                return;
            }
            usize at = m_entries.Size();
            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                if (m_entries[i].order > order)
                {
                    at = i;
                    break;
                }
            }
            m_entries.Insert(at, Entry{modifier, order});
        }

        void Remove(IPoseModifier* modifier)
        {
            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                if (m_entries[i].modifier == modifier)
                {
                    m_entries.RemoveAt(i);
                    return;
                }
            }
        }

        [[nodiscard]] bool Contains(const IPoseModifier* modifier) const noexcept
        {
            for (const Entry& e : m_entries)
            {
                if (e.modifier == modifier)
                {
                    return true;
                }
            }
            return false;
        }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_entries.IsEmpty(); }
        [[nodiscard]] usize Count() const noexcept { return m_entries.Size(); }
        [[nodiscard]] const ModelPoseCache& Model() const noexcept { return m_model; }

        /// Every modifier over `localPoses`, in order, the model-space pose built once first.
        void Apply(const Skeleton& skeleton, Span<BoneTransform> localPoses)
        {
            if (m_entries.IsEmpty())
            {
                return;
            }
            m_model.Build(skeleton, Span<const BoneTransform>{localPoses.Data(), localPoses.Size()});
            for (const Entry& e : m_entries)
            {
                e.modifier->Apply(skeleton, localPoses, m_model);
            }
        }

    private:
        struct Entry
        {
            IPoseModifier* modifier = nullptr;
            i32 order = 0;
        };
        Array<Entry> m_entries;
        ModelPoseCache m_model;
    };
}
