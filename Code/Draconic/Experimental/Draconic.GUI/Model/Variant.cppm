// Draconic GUI - :variant partition
//
// Variant: a small tagged value a Model hands to a view for a cell - the currency of the MVC
// layer. Modeled on eepp's Models::Variant (role only, common subset): empty / bool / int /
// float / string / color. Stored as a plain struct (no union) for simplicity and safety;
// ToString renders it for display and Compare orders it for sorting.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:variant;

import draconic.foundation; // String, StringView, Color, i64, f64

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class Variant
    {
    public:
        enum class Type
        {
            Empty,
            Bool,
            Int,
            Float,
            String,
            Color
        };

        Variant() = default;
        Variant(bool value) : m_type(Type::Bool), m_bool(value) {}
        Variant(i64 value) : m_type(Type::Int), m_int(value) {}
        Variant(i32 value) : m_type(Type::Int), m_int(value) {}
        Variant(f64 value) : m_type(Type::Float), m_float(value) {}
        Variant(foundation::StringView value) : m_type(Type::String), m_string(value) {}
        Variant(const foundation::String& value) : m_type(Type::String), m_string(value) {}
        Variant(foundation::Color value) : m_type(Type::Color), m_color(value) {}

        [[nodiscard]] Type GetType() const noexcept { return m_type; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_type == Type::Empty; }

        [[nodiscard]] bool AsBool() const noexcept
        {
            return m_type == Type::Bool ? m_bool : (m_type == Type::Int ? m_int != 0 : false);
        }
        [[nodiscard]] i64 AsInt() const noexcept
        {
            return m_type == Type::Int ? m_int
                                       : (m_type == Type::Float ? static_cast<i64>(m_float) : 0);
        }
        [[nodiscard]] f64 AsFloat() const noexcept
        {
            return m_type == Type::Float ? m_float
                                         : (m_type == Type::Int ? static_cast<f64>(m_int) : 0.0);
        }
        [[nodiscard]] foundation::StringView AsString() const noexcept { return m_string.AsView(); }
        [[nodiscard]] foundation::Color AsColor() const noexcept { return m_color; }

        // A display string for any type (numbers rendered simply; string returned as-is).
        [[nodiscard]] foundation::String ToString() const
        {
            switch (m_type)
            {
            case Type::Empty:
                return foundation::String{};
            case Type::Bool:
                return foundation::String(m_bool ? foundation::StringView(u8"true")
                                           : foundation::StringView(u8"false"));
            case Type::Int:
                return IntToString(m_int);
            case Type::Float:
                return FloatToString(m_float);
            case Type::String:
                return m_string;
            case Type::Color:
                return foundation::String{}; // colors have no textual form here
            }
            return foundation::String{};
        }

        // Order two variants: numbers by value, strings lexicographically, bools false<true.
        // Mixed/empty types compare by their Type ordinal (a stable, if arbitrary, total order).
        [[nodiscard]] i32 Compare(const Variant& other) const
        {
            if (m_type != other.m_type)
                return static_cast<i32>(m_type) < static_cast<i32>(other.m_type) ? -1 : 1;
            switch (m_type)
            {
            case Type::Empty:
                return 0;
            case Type::Bool:
                return (m_bool == other.m_bool) ? 0 : (!m_bool ? -1 : 1);
            case Type::Int:
                return (m_int == other.m_int) ? 0 : (m_int < other.m_int ? -1 : 1);
            case Type::Float:
                return (m_float == other.m_float) ? 0 : (m_float < other.m_float ? -1 : 1);
            case Type::String:
                return CompareStrings(m_string.AsView(), other.m_string.AsView());
            case Type::Color:
                return 0;
            }
            return 0;
        }

    private:
        [[nodiscard]] static i32 CompareStrings(foundation::StringView a, foundation::StringView b) noexcept
        {
            const usize n = a.Size() < b.Size() ? a.Size() : b.Size();
            for (usize i = 0; i < n; ++i)
                if (a[i] != b[i])
                    return a[i] < b[i] ? -1 : 1;
            if (a.Size() == b.Size())
                return 0;
            return a.Size() < b.Size() ? -1 : 1;
        }

        [[nodiscard]] static foundation::String IntToString(i64 value)
        {
            char8_t buf[24];
            usize pos = 24;
            const bool neg = value < 0;
            u64 v = neg ? static_cast<u64>(-(value + 1)) + 1u : static_cast<u64>(value);
            if (v == 0)
                buf[--pos] = u8'0';
            while (v > 0)
            {
                buf[--pos] = static_cast<char8_t>(u8'0' + (v % 10));
                v /= 10;
            }
            if (neg)
                buf[--pos] = u8'-';
            return foundation::String(foundation::StringView(buf + pos, 24 - pos));
        }

        // Compact fixed-ish float rendering (integer part + up to 3 decimals, trailing zeros trimmed).
        [[nodiscard]] static foundation::String FloatToString(f64 value)
        {
            const bool neg = value < 0.0;
            f64 v = neg ? -value : value;
            const i64 whole = static_cast<i64>(v);
            f64 frac = v - static_cast<f64>(whole);
            foundation::String out;
            if (neg)
                out.Append(foundation::StringView(u8"-"));
            out += IntToString(whole);
            // up to 3 decimal digits
            char8_t digits[3];
            usize dc = 0;
            for (usize i = 0; i < 3; ++i)
            {
                frac *= 10.0;
                const i32 d = static_cast<i32>(frac);
                digits[dc++] = static_cast<char8_t>(u8'0' + d);
                frac -= d;
            }
            while (dc > 0 && digits[dc - 1] == u8'0')
                --dc; // trim trailing zeros
            if (dc > 0)
            {
                out.Append(foundation::StringView(u8"."));
                out.Append(foundation::StringView(digits, dc));
            }
            return out;
        }

        Type m_type = Type::Empty;
        bool m_bool = false;
        i64 m_int = 0;
        f64 m_float = 0.0;
        foundation::String m_string;
        foundation::Color m_color{};
    };
}
