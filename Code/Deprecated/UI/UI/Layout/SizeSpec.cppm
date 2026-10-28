// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :size_spec partition
//
// How a view should be sized along one axis (Fixed(Unit) / Match / Wrap). Stored on LayoutParams.
// Ported from Sedulous.UI/src/Layout/SizeSpec.bf (discriminated union -> kind + payload struct).

module;
#include "Core/Prelude.h"

export module foundation.ui:size_spec;

import foundation.core;
import :unit;

using namespace foundation::core;

export namespace foundation::ui
{
    struct SizeSpec
    {
        enum class Kind
        {
            Fixed,
            Match,
            Wrap
        };

        Kind kind = Kind::Wrap;
        Unit fixedSize{};

        constexpr SizeSpec() noexcept = default; // default: Wrap

        /// Explicit size with a unit (dp/pt/px/%/em, or a calc sum of them).
        [[nodiscard]] static constexpr SizeSpec Fixed(Unit size) noexcept
        {
            SizeSpec s;
            s.kind = Kind::Fixed;
            s.fixedSize = size;
            return s;
        }
        /// Fill the parent's available space.
        [[nodiscard]] static constexpr SizeSpec Match() noexcept
        {
            SizeSpec s;
            s.kind = Kind::Match;
            return s;
        }
        /// Fit to the view's content (intrinsic size).
        [[nodiscard]] static constexpr SizeSpec Wrap() noexcept
        {
            SizeSpec s;
            s.kind = Kind::Wrap;
            return s;
        }

        /// For Fixed: resolves the ABSOLUTE unit components. For Match/Wrap: 0 (parent handles
        /// these). Percent/em components need the three-argument overload.
        [[nodiscard]] constexpr f32 ResolveFixed(f32 dpiScale) const noexcept
        {
            return kind == Kind::Fixed ? fixedSize.Resolve(dpiScale) : 0.0f;
        }
        /// For Fixed: resolves every component - `referenceSize` is the containing box on this
        /// axis (0 when unbounded), `fontSize` the computed font size for em.
        [[nodiscard]] constexpr f32 ResolveFixed(f32 dpiScale, f32 referenceSize,
                                                 f32 fontSize) const noexcept
        {
            return kind == Kind::Fixed ? fixedSize.Resolve(dpiScale, referenceSize, fontSize) : 0.0f;
        }

        /// Whether this is a fixed size (not Match or Wrap).
        [[nodiscard]] constexpr bool IsFixed() const noexcept { return kind == Kind::Fixed; }

        [[nodiscard]] constexpr bool operator==(const SizeSpec&) const noexcept = default;
    };
}
