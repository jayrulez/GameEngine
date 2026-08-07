/// Draconic::Animation - the `:skeleton` partition.
///
/// The skeletal hierarchy (ported faithfully from Sedulous.Animation.Skeleton/Bone). A `Bone` is a
/// node with a local bind pose + inverse bind matrix; the `Skeleton` owns the bones and computes
/// world-space + skinning matrices from a set of local poses, evaluated parents-before-children.
///
/// `BoneTransform` reuses `foundation::Transform` (position/rotation/scale, S*R*T) - byte-for-byte the
/// Sedulous BoneTransform, with Lerp/ToMatrix already in foundation math.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.animation:skeleton;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::animation
{

    // A compact per-bone transform for animation (pos/rot/scale). Reuses the foundation math type.
    using BoneTransform = foundation::Transform;

    // One bone in a skeleton hierarchy. Value type owned by the Skeleton's bone array (no per-bone heap
    // allocation, unlike the Beef original's Bone[] of references).
    struct Bone
    {
        String name;
        i32 index = 0;
        i32 parentIndex = -1;             // -1 = root
        BoneTransform localBindPose = {}; // local transform relative to parent (bind pose)
        Float4x4 inverseBindPose = Float4x4::Identity(); // model space -> bone space
        Float4x4 rootCorrection =
            Float4x4::Identity(); // missing-ancestor transform for roots (e.g. FBX axis conv)
        Array<i32> children;
    };

    // A resource product (Object), so a cooked SkeletonSource can build into it via the resource system.
    class Skeleton : public Object
    {
        DRACONIC_OBJECT(Skeleton, Object)
    public:
        Skeleton() = default;
        // Creates `boneCount` default bones with sequential indices (the loader fills the rest).
        explicit Skeleton(i32 boneCount)
        {
            m_bones.Resize(boneCount < 0 ? 0 : static_cast<usize>(boneCount));
            for (usize i = 0; i < m_bones.Size(); ++i)
            {
                m_bones[i].index = static_cast<i32>(i);
            }
        }

        Skeleton(const Skeleton&) = delete;
        Skeleton& operator=(const Skeleton&) = delete;

        [[nodiscard]] i32 BoneCount() const noexcept { return static_cast<i32>(m_bones.Size()); }
        [[nodiscard]] Array<Bone>& Bones() noexcept { return m_bones; }
        [[nodiscard]] const Array<Bone>& Bones() const noexcept { return m_bones; }
        [[nodiscard]] Span<const i32> RootBones() const noexcept
        {
            return {m_rootBones.Data(), m_rootBones.Size()};
        }
        [[nodiscard]] String& Name() noexcept { return m_name; }
        [[nodiscard]] const String& Name() const noexcept { return m_name; }

        // Re-populate this same instance (resource hot-reload keeps outside references valid).
        void ClearForReload(i32 boneCount)
        {
            m_bones.Clear();
            m_rootBones.Clear();
            m_hierarchicalOrder.Clear();
            m_nameMap.Clear();
            m_name.Clear();
            m_bones.Resize(boneCount < 0 ? 0 : static_cast<usize>(boneCount));
            for (usize i = 0; i < m_bones.Size(); ++i)
            {
                m_bones[i].index = static_cast<i32>(i);
            }
        }

        // Bone index by name, or -1 if not found (requires BuildNameMap()).
        [[nodiscard]] i32 FindBone(StringView name) const
        {
            if (const i32* idx = m_nameMap.Find(String{name}))
            {
                return *idx;
            }
            return -1;
        }

        [[nodiscard]] Bone* GetBone(i32 index)
        {
            return InBounds(index) ? &m_bones[static_cast<usize>(index)] : nullptr;
        }
        [[nodiscard]] const Bone* GetBone(i32 index) const
        {
            return InBounds(index) ? &m_bones[static_cast<usize>(index)] : nullptr;
        }

        // Build the name->index lookup. Call after all bones + names are set.
        void BuildNameMap()
        {
            m_nameMap.Clear();
            for (const Bone& b : m_bones)
            {
                if (!b.name.IsEmpty())
                {
                    m_nameMap.InsertOrAssign(b.name, b.index);
                }
            }
        }

        // Cache root-bone indices (parentIndex < 0). Call after parents are set.
        void FindRootBones()
        {
            m_rootBones.Clear();
            for (const Bone& b : m_bones)
            {
                if (b.parentIndex < 0)
                {
                    m_rootBones.PushBack(b.index);
                }
            }
        }

        // Build each bone's child-index list + the parents-before-children evaluation order. Call after
        // parents are set (and after FindRootBones for a correct hierarchical order).
        void BuildChildIndices()
        {
            const usize n = m_bones.Size();
            Array<i32> childCounts;
            childCounts.Resize(n);
            for (usize i = 0; i < n; ++i)
            {
                childCounts[i] = 0;
            }
            for (const Bone& b : m_bones)
            {
                if (b.parentIndex >= 0 && InBounds(b.parentIndex))
                {
                    ++childCounts[static_cast<usize>(b.parentIndex)];
                }
            }

            for (usize i = 0; i < n; ++i)
            {
                m_bones[i].children.Clear();
                m_bones[i].children.Reserve(static_cast<usize>(childCounts[i]));
            }
            for (const Bone& b : m_bones)
            {
                if (b.parentIndex >= 0 && InBounds(b.parentIndex))
                {
                    m_bones[static_cast<usize>(b.parentIndex)].children.PushBack(b.index);
                }
            }
            BuildHierarchicalOrder();
        }

        // World bind pose -> inverse bind matrix for each bone. Call after local bind poses + parents set.
        void ComputeInverseBindPoses()
        {
            const usize n = m_bones.Size();
            m_worldScratch.Resize(n);
            ComputeWorldPoses(Span<const BoneTransform>{},
                              Span<Float4x4>{m_worldScratch.Data(), n}); // bind pose
            for (usize i = 0; i < n; ++i)
            {
                m_bones[i].inverseBindPose = Inverse(m_worldScratch[i]);
            }
        }

        // World-space matrices from local transforms (empty `localPoses` => bind pose), parents first.
        void ComputeWorldPoses(Span<const BoneTransform> localPoses, Span<Float4x4> outWorldPoses)
        {
            if (!m_hierarchicalOrder.IsEmpty())
            {
                for (i32 boneIndex : m_hierarchicalOrder)
                {
                    ComputeBoneWorldPose(boneIndex, localPoses, outWorldPoses);
                }
            }
            else
            {
                for (i32 i = 0; i < BoneCount(); ++i)
                {
                    ComputeBoneWorldPose(i, localPoses, outWorldPoses);
                }
            }
        }

        // Final skinning matrices = inverseBindPose * worldPose (row-vector: vertex * skin = v * IBM * world).
        void ComputeSkinningMatrices(Span<const BoneTransform> localPoses,
                                     Span<Float4x4> outSkinningMatrices)
        {
            const usize n = m_bones.Size();
            m_worldScratch.Resize(n);
            ComputeWorldPoses(localPoses, Span<Float4x4>{m_worldScratch.Data(), n});
            for (usize i = 0; i < n; ++i)
            {
                if (i < outSkinningMatrices.Size())
                {
                    outSkinningMatrices[i] = m_bones[i].inverseBindPose * m_worldScratch[i];
                }
            }
        }

    private:
        [[nodiscard]] bool InBounds(i32 i) const noexcept
        {
            return i >= 0 && static_cast<usize>(i) < m_bones.Size();
        }

        void ComputeBoneWorldPose(i32 boneIndex, Span<const BoneTransform> localPoses,
                                  Span<Float4x4> outWorldPoses)
        {
            const usize bi = static_cast<usize>(boneIndex);
            if (!InBounds(boneIndex) || bi >= outWorldPoses.Size())
            {
                return;
            }
            const Bone& bone = m_bones[bi];

            const BoneTransform local =
                (bi < localPoses.Size()) ? localPoses[bi] : bone.localBindPose;
            const Float4x4 localMatrix = local.ToMatrix();

            if (bone.parentIndex >= 0 && InBounds(bone.parentIndex) &&
                static_cast<usize>(bone.parentIndex) < outWorldPoses.Size())
            {
                outWorldPoses[bi] =
                    localMatrix *
                    outWorldPoses[static_cast<usize>(bone.parentIndex)]; // child = local * parent
            }
            else
            {
                outWorldPoses[bi] = localMatrix * bone.rootCorrection;
            }
        }

        // BFS over the hierarchy so parents precede children; orphans appended at the end.
        void BuildHierarchicalOrder()
        {
            const usize n = m_bones.Size();
            m_hierarchicalOrder.Clear();
            m_hierarchicalOrder.Reserve(n);

            Array<i32> queue;
            for (i32 root : m_rootBones)
            {
                queue.PushBack(root);
            }
            usize head = 0;
            while (head < queue.Size())
            {
                const i32 boneIndex = queue[head++];
                m_hierarchicalOrder.PushBack(boneIndex);
                if (InBounds(boneIndex))
                {
                    for (i32 child : m_bones[static_cast<usize>(boneIndex)].children)
                    {
                        queue.PushBack(child);
                    }
                }
            }

            // Append any orphans not reached from a root.
            if (m_hierarchicalOrder.Size() < n)
            {
                for (i32 i = 0; i < static_cast<i32>(n); ++i)
                {
                    bool found = false;
                    for (i32 ordered : m_hierarchicalOrder)
                    {
                        if (ordered == i)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        m_hierarchicalOrder.PushBack(i);
                    }
                }
            }
        }

        String m_name;
        Array<Bone> m_bones;
        Array<i32> m_rootBones;
        Array<i32> m_hierarchicalOrder;
        HashMap<String, i32> m_nameMap;
        Array<Float4x4> m_worldScratch; // reused world-pose scratch (skinning hot path)
    };

    DRACONIC_DEFINE_OBJECT(Skeleton, "draconic::animation")

} // namespace draconic::animation
