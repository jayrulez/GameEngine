// Draconic UI - :size_spec partition
//
// How a view should be sized along one axis (Fixed(Unit) / Match / Wrap). Stored on LayoutParams.
// Ported from Sedulous.UI/src/Layout/SizeSpec.bf (discriminated union -> kind + payload struct).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:size_spec;

import draconic.foundation;
import :unit;

using namespace draconic::foundation;

export namespace draconic::ui
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

        /// Explicit size with a unit (dp/pt/px).
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

        /// For Fixed: resolves the unit. For Match/Wrap: 0 (parent handles these).
        [[nodiscard]] constexpr f32 ResolveFixed(f32 dpiScale) const noexcept
        {
            return kind == Kind::Fixed ? fixedSize.Resolve(dpiScale) : 0.0f;
        }

        /// Whether this is a fixed size (not Match or Wrap).
        [[nodiscard]] constexpr bool IsFixed() const noexcept { return kind == Kind::Fixed; }
    };
}
