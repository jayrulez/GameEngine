// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Animation - the `:pose` partition.
///
/// AnimationPose: a non-owning view over per-bone local transforms (and optional morph weights)
/// representing one evaluated pose. The backing arrays must outlive the view.

module;
#include "Core/Prelude.h"

export module foundation.animation:pose;

import foundation.core;
import :skeleton; // BoneTransform

using namespace foundation::core;

export namespace foundation::animation
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

} // namespace foundation::animation
