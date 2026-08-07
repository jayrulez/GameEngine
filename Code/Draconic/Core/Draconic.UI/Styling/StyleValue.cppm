// Draconic UI - :style_value partition
//
// A tagged value stored in a StyleRule: a discriminated union of Color / Float / Thickness /
// Drawable / Bool / String / None. Ported from Sedulous.UI/src/Styling/StyleValue.bf.
//
// Divergence (language): Beef discriminated union -> a kind + separate storage members. This makes
// the ownership Beef managed by hand (DrawableRef AddRef/Release, StringRef copy/delete) automatic:
// the Drawable is a RefPtr<Drawable> and the string is an owned String, so copy/move/destroy of a
// StyleValue is correct with no manual refcounting.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:style_value;

import draconic.foundation; // Color, Optional, RefPtr, String, StringView
import :thickness;
import :drawable;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class StyleValue
    {
    public:
        enum class Kind
        {
            None,
            Color,
            Float,
            Thickness,
            Drawable,
            Bool,
            String
        };

        StyleValue() = default; // None

        [[nodiscard]] static StyleValue ColorVal(foundation::Color c)
        {
            StyleValue v;
            v.m_kind = Kind::Color;
            v.m_color = c;
            return v;
        }
        [[nodiscard]] static StyleValue FloatVal(f32 f)
        {
            StyleValue v;
            v.m_kind = Kind::Float;
            v.m_float = f;
            return v;
        }
        [[nodiscard]] static StyleValue ThicknessVal(Thickness t)
        {
            StyleValue v;
            v.m_kind = Kind::Thickness;
            v.m_thickness = t;
            return v;
        }
        [[nodiscard]] static StyleValue DrawableRef(RefPtr<Drawable> d)
        {
            StyleValue v;
            v.m_kind = Kind::Drawable;
            v.m_drawable = Move(d);
            return v;
        }
        [[nodiscard]] static StyleValue BoolVal(bool b)
        {
            StyleValue v;
            v.m_kind = Kind::Bool;
            v.m_bool = b;
            return v;
        }
        [[nodiscard]] static StyleValue StringRef(StringView s)
        {
            StyleValue v;
            v.m_kind = Kind::String;
            v.m_string = String(s);
            return v;
        }
        [[nodiscard]] static StyleValue None() { return StyleValue{}; }

        [[nodiscard]] Kind GetKind() const noexcept { return m_kind; }

        /// Try to get as Color / Float / Thickness / Bool (empty Optional if the kind differs).
        [[nodiscard]] Optional<foundation::Color> AsColor() const
        {
            if (m_kind == Kind::Color)
            {
                return m_color;
            }
            return {};
        }
        [[nodiscard]] Optional<f32> AsFloat() const
        {
            if (m_kind == Kind::Float)
            {
                return m_float;
            }
            return {};
        }
        [[nodiscard]] Optional<Thickness> AsThickness() const
        {
            if (m_kind == Kind::Thickness)
            {
                return m_thickness;
            }
            return {};
        }
        [[nodiscard]] Optional<bool> AsBool() const
        {
            if (m_kind == Kind::Bool)
            {
                return m_bool;
            }
            return {};
        }

        /// Borrowed drawable pointer (owned by this value), or null if the kind differs.
        [[nodiscard]] Drawable* AsDrawable() const
        {
            return m_kind == Kind::Drawable ? m_drawable.Get() : nullptr;
        }
        /// Borrowed string view (backing owned by this value), or empty if the kind differs.
        [[nodiscard]] Optional<StringView> AsString() const
        {
            if (m_kind == Kind::String)
            {
                return m_string.AsView();
            }
            return {};
        }

    private:
        Kind m_kind = Kind::None;
        foundation::Color m_color{};
        f32 m_float = 0.0f;
        Thickness m_thickness{};
        RefPtr<Drawable> m_drawable;
        bool m_bool = false;
        String m_string;
    };
}
