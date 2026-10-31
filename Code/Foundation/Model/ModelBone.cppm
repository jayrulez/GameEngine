// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// A bone/node in the model hierarchy with TRS decomposition.

module;
#include "Core/Prelude.h"

#include <cmath>
#include <string>
#include <vector>

export module foundation.model:model_bone;

import foundation.core;

using namespace foundation::core;

export namespace foundation::model
{

    /// A bone/node in the model skeleton hierarchy.
    class ModelBone
    {
    public:
        ModelBone() = default;
        ~ModelBone() = default;

        [[nodiscard]] StringView name() const { return StringView(m_name.Data(), m_name.Size()); }
        void setName(StringView n) { m_name = String(n); }

        /// Add a child bone (non-owning pointer).
        void addChild(ModelBone* child) { m_children.PushBack(child); }

        /// Remove all children (does not delete them; children are non-owning).
        void clearChildren() { m_children.Clear(); }

        /// Non-owning child pointers (owned by Model::m_bones).
        [[nodiscard]] Span<ModelBone* const> children() const
        {
            return Span<ModelBone* const>(m_children.Data(), m_children.Size());
        }
        [[nodiscard]] Span<ModelBone*> children()
        {
            return Span<ModelBone*>(m_children.Data(), m_children.Size());
        }

        /// Update localTransform from translation, rotation and scale (TRS), with the same
        /// composition as Transform::ToMatrix (row-vector convention: Scale * Rotation with
        /// the translation in the last row), so it agrees with Float4x4::TransformPoint and
        /// with a glTF node matrix read into row-major storage. A column-vector matrix
        /// (translation in the last column, T * (R * S)) would not.
        void updateLocalTransform()
        {
            localTransform = Transform{translation, rotation, scale}.ToMatrix();
        }

        // -- Public fields --

        /// Index of this bone in the model's bone array.
        i32 index = 0;

        /// Parent bone index (-1 if root).
        i32 parentIndex = -1;

        /// Local transform relative to parent.
        Float4x4 localTransform = Float4x4::Identity();

        /// Inverse bind matrix for skinning (mesh space -> bone space).
        Float4x4 inverseBindMatrix = Float4x4::Identity();

        /// Translation component of local transform.
        Float3 translation{};

        /// Rotation component of local transform (quaternion).
        Quaternion rotation = Quaternion::Identity;

        /// Scale component of local transform.
        Float3 scale{1, 1, 1};

        /// Mesh index if this node has a mesh (-1 if none).
        i32 meshIndex = -1;

        /// Skin index if this is a skinned mesh node (-1 if none).
        i32 skinIndex = -1;

        /// Whether the file makes this node a bone of its skeleton (an FBX limb or root node), so a
        /// helper bone the skin's joints hang from (3ds Max's Dummy, animated with the pose) joins the
        /// skeleton, where an empty or null node they hang from (Blender's armature object) does not.
        bool isSkeletonBone = false;

    private:
        String m_name;
        Array<ModelBone*> m_children;
    };

} // namespace foundation::model
