// Draconic UI - :input_filter partition
//
// Filters characters before insertion into a text control. Ported from
// Sedulous.UI/src/Editing/InputFilter.bf. Self-contained (no View dependency); a plain value type
// held by the text-editing behavior. The Beef `delegate bool(char32)` becomes Function<bool(char32_t)>;
// factory methods return by value (no heap).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:input_filter;

import draconic.foundation; // Function

using namespace draconic::foundation;

export namespace draconic::ui
{
    /// Predefined input filter modes.
    enum class InputFilterMode
    {
        None,
        Digits,
        HexDigits,
        Custom
    };

    class InputFilter
    {
    public:
        InputFilter() = default;

        [[nodiscard]] InputFilterMode Mode() const noexcept { return m_mode; }
        void SetMode(InputFilterMode mode) noexcept { m_mode = mode; }

        /// Set a custom character predicate (switches mode to Custom).
        void SetCustomFilter(Function<bool(char32_t)> predicate)
        {
            m_customPredicate = Move(predicate);
            m_mode = InputFilterMode::Custom;
        }

        /// True if the character is accepted by this filter.
        [[nodiscard]] bool Accept(char32_t c)
        {
            switch (m_mode)
            {
            case InputFilterMode::None:
                return true;
            case InputFilterMode::Digits:
                return c >= U'0' && c <= U'9';
            case InputFilterMode::HexDigits:
                return (c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'f') ||
                       (c >= U'A' && c <= U'F');
            case InputFilterMode::Custom:
                return m_customPredicate ? m_customPredicate(c) : true;
            }
            return true;
        }

        /// A digits-only filter.
        [[nodiscard]] static InputFilter Digits()
        {
            InputFilter f;
            f.m_mode = InputFilterMode::Digits;
            return f;
        }
        /// A hex-digits filter.
        [[nodiscard]] static InputFilter HexDigits()
        {
            InputFilter f;
            f.m_mode = InputFilterMode::HexDigits;
            return f;
        }

    private:
        InputFilterMode m_mode = InputFilterMode::None;
        Function<bool(char32_t)> m_customPredicate;
    };
}
