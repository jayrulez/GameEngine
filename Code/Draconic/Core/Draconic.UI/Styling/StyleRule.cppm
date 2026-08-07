// Draconic UI - :style_rule partition
//
// A single style rule: a selector plus a set of property assignments. Ported from
// Sedulous.UI/src/Styling/StyleRule.bf.
//
// Divergence (language): Beef's manual AddRef/Release/delete lifecycle for DrawableRef/StringRef
// values disappears - StyleValue owns its RefPtr<Drawable>/String, so overwriting or destroying an
// entry releases the old resource automatically. The `consumeRef` parameter is gone (pass a
// RefPtr<Drawable>; ownership is by-value).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:style_rule;

import draconic.foundation; // Object, Array, Optional, RefPtr, Color, StringView
import :thickness;
import :drawable;
import :style_property;
import :style_value;
import :style_selector;

using namespace draconic::foundation;

export namespace draconic::ui
{
    // Object (RefCounted) so a StyleSheet can own rules via RefPtr and return stable references
    // from its fluent builders.
    class StyleRule : public Object
    {
        DRACONIC_OBJECT(StyleRule, Object)
    public:
        struct Entry
        {
            StyleProperty Prop;
            StyleValue Value;
        };

        StyleSelector Selector;

        StyleRule() = default;

        StyleRule& Set(StyleProperty prop, foundation::Color color)
        {
            SetOverwrite(prop, StyleValue::ColorVal(color));
            return *this;
        }
        StyleRule& Set(StyleProperty prop, f32 value)
        {
            SetOverwrite(prop, StyleValue::FloatVal(value));
            return *this;
        }
        StyleRule& Set(StyleProperty prop, Thickness value)
        {
            SetOverwrite(prop, StyleValue::ThicknessVal(value));
            return *this;
        }
        StyleRule& Set(StyleProperty prop, RefPtr<Drawable> drawable)
        {
            SetOverwrite(prop, StyleValue::DrawableRef(Move(drawable)));
            return *this;
        }
        StyleRule& Set(StyleProperty prop, bool value)
        {
            SetOverwrite(prop, StyleValue::BoolVal(value));
            return *this;
        }
        StyleRule& Set(StyleProperty prop, StringView value)
        {
            SetOverwrite(prop, StyleValue::StringRef(value));
            return *this;
        }

        /// Remove a property (releases any owned resource). No-op if not set.
        bool Remove(StyleProperty prop)
        {
            for (usize i = 0; i < m_properties.Size(); ++i)
            {
                if (m_properties[i].Prop == prop)
                {
                    m_properties.RemoveAt(i);
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] usize PropertyCount() const noexcept { return m_properties.Size(); }
        [[nodiscard]] const Entry& GetProperty(usize index) const { return m_properties[index]; }

        /// Try to find a specific property in this rule.
        [[nodiscard]] Optional<StyleValue> GetValue(StyleProperty prop) const
        {
            for (const Entry& e : m_properties)
            {
                if (e.Prop == prop)
                {
                    return e.Value;
                }
            }
            return {};
        }

    private:
        void SetOverwrite(StyleProperty prop, StyleValue value)
        {
            for (Entry& e : m_properties)
            {
                if (e.Prop == prop)
                {
                    e.Value = Move(value);
                    return;
                }
            }
            m_properties.PushBack(Entry{prop, Move(value)});
        }

        Array<Entry> m_properties;
    };

    DRACONIC_DEFINE_OBJECT(StyleRule, "draconic::ui")
}
