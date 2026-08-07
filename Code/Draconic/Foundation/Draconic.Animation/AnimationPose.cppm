/// Draconic::Animation - the `:pose` partition.
///
/// AnimationPose: a non-owning view over per-bone local transforms (+ optional morph weights)
/// representing one evaluated pose. The backing arrays must outlive the view. Ported faithfully from
/// Sedulous.Animation.AnimationPose.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.animation:pose;

import draconic.foundation;
import :skeleton; // BoneTransform

using namespace draconic::foundation;

export namespace draconic::animation
{

    struct AnimationPose
    {
        Span<BoneTransform> boneTransforms; // per-bone local transforms
        Span<f32> morphWeights;             // per-morph-target weights (empty until morph support)

        AnimationPose() = default;
        explicit AnimationPose(Span<BoneTransform> bones, Span<f32> morphs = {}) noexcept
            : boneTransforms(bones), morphWeights(morphs)
        {
        }

        [[nodiscard]] usize BoneCount() const noexcept { return boneTransforms.Size(); }
        [[nodiscard]] bool HasMorphWeights() const noexcept { return morphWeights.Size() > 0; }
    };

} // namespace draconic::animation
