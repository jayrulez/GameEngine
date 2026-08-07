/// A bone/node in the model hierarchy with TRS decomposition.
/// Ported from Sedulous.Models/ModelBone.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include <cmath>
#include <string>
#include <vector>

export module draconic.model:model_bone;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::model
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

        /// Remove all children (does not delete -- children are non-owning).
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

        /// Update localTransform from translation/rotation/scale (TRS).
        /// Order: Scale -> Rotate -> Translate.
        void updateLocalTransform()
        {
            // Build scale matrix.
            Float4x4 s = Float4x4::Identity();
            s.m[0][0] = scale.x;
            s.m[1][1] = scale.y;
            s.m[2][2] = scale.z;

            // Build rotation matrix from quaternion.
            f32 xx = rotation.x * rotation.x;
            f32 yy = rotation.y * rotation.y;
            f32 zz = rotation.z * rotation.z;
            f32 xy = rotation.x * rotation.y;
            f32 xz = rotation.x * rotation.z;
            f32 yz = rotation.y * rotation.z;
            f32 wx = rotation.w * rotation.x;
            f32 wy = rotation.w * rotation.y;
            f32 wz = rotation.w * rotation.z;

            Float4x4 r = Float4x4::Identity();
            r.m[0][0] = 1.0f - 2.0f * (yy + zz);
            r.m[0][1] = 2.0f * (xy + wz);
            r.m[0][2] = 2.0f * (xz - wy);
            r.m[1][0] = 2.0f * (xy - wz);
            r.m[1][1] = 1.0f - 2.0f * (xx + zz);
            r.m[1][2] = 2.0f * (yz + wx);
            r.m[2][0] = 2.0f * (xz + wy);
            r.m[2][1] = 2.0f * (yz - wx);
            r.m[2][2] = 1.0f - 2.0f * (xx + yy);

            // Build translation matrix.
            Float4x4 t = Float4x4::Identity();
            t.m[0][3] = translation.x;
            t.m[1][3] = translation.y;
            t.m[2][3] = translation.z;

            // TRS order: Scale -> Rotate -> Translate.
            localTransform = t * (r * s);
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

    private:
        String m_name;
        Array<ModelBone*> m_children;
    };

} // namespace draconic::model
