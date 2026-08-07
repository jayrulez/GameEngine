/// Skin data for skeletal animation.
/// Ported from Sedulous.Models/ModelSkin.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include <string>
#include <vector>

export module draconic.model:model_skin;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::model
{

    /// Skin data binding joints to inverse bind matrices.
    class ModelSkin
    {
    public:
        ModelSkin() = default;
        ~ModelSkin() = default;

        [[nodiscard]] StringView name() const { return StringView(m_name.Data(), m_name.Size()); }
        void setName(StringView n) { m_name = String(n); }

        /// Add a joint to the skin.
        void addJoint(i32 boneIndex, Float4x4 inverseBindMatrix)
        {
            m_joints.PushBack(boneIndex);
            m_inverseBindMatrices.PushBack(inverseBindMatrix);
        }

        [[nodiscard]] Span<const i32> joints() const
        {
            return Span<const i32>(m_joints.Data(), m_joints.Size());
        }
        [[nodiscard]] Span<i32> joints() { return Span<i32>(m_joints.Data(), m_joints.Size()); }

        [[nodiscard]] Span<const Float4x4> inverseBindMatrices() const
        {
            return Span<const Float4x4>(m_inverseBindMatrices.Data(), m_inverseBindMatrices.Size());
        }
        [[nodiscard]] Span<Float4x4> inverseBindMatrices()
        {
            return Span<Float4x4>(m_inverseBindMatrices.Data(), m_inverseBindMatrices.Size());
        }

        // -- Public fields --

        /// Index of the skeleton root bone (-1 if not specified).
        i32 skeletonRootIndex = -1;

    private:
        String m_name;
        Array<i32> m_joints;
        Array<Float4x4> m_inverseBindMatrices;
    };

} // namespace draconic::model
