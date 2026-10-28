// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :style_rule partition
//
// A single style rule: a selector plus a set of property assignments. Ported from
// Sedulous.UI/src/Styling/StyleRule.bf.
//
// Divergence (language): Beef's manual AddRef/Release/delete lifecycle for DrawableRef/StringRef
// values disappears - StyleValue owns its RefPtr<Drawable>/String, so overwriting or destroying an
// entry releases the old resource automatically. The `consumeRef` parameter is gone (pass a
// RefPtr<Drawable>; ownership is by-value).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:style_rule;

import foundation.core; // Object, Array, Optional, RefPtr, Color, StringView
import :thickness;
import :drawable;
import :style_property;
import :style_value;
import :style_selector;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui
{
    // Object (RefCounted) so a StyleSheet can own rules via RefPtr and return stable references
    // from its fluent builders.
    class StyleRule : public Object
    {
        RTTI_OBJECT(StyleRule, Object)
    public:
        struct Entry
        {
            StyleProperty Prop;
            StyleValue Value;
        };

        StyleSelector Selector;

        StyleRule() = default;

        /// The owning sheet's version counter (set by the sheet on AddRule; re-homed by
        /// MergeFrom; cleared when the sheet dies). A rule edit bumps it, so only the views
        /// whose chain includes THAT sheet rebuild their computed style - a game context's
        /// sheet edits never touch the editor's caches, and an inline style edit on one view
        /// touches that view alone.
        void BindOwnerVersion(u32* version) noexcept { m_ownerVersion = version; }
        [[nodiscard]] u32* OwnerVersion() const noexcept { return m_ownerVersion; }

        StyleRule& Set(StyleProperty prop, core::Color color)
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
        /// Any value, including Length / Inherit / Initial / Variable references.
        StyleRule& SetValue(StyleProperty prop, StyleValue value)
        {
            SetOverwrite(prop, Move(value));
            return *this;
        }

        // === Custom properties (`--name: value`) ===
        // Named, untyped values the cascade carries like any declaration; `var(--name)` and
        // View::CustomProperty read them (the custom-layout extension point).
        struct CustomEntry
        {
            String Name; ///< including the leading `--`
            u64 NameHash = 0;
            StyleValue Value;
        };
        StyleRule& SetCustom(StringView name, StyleValue value)
        {
            const u64 hash = HashText(name);
            for (CustomEntry& e : m_custom)
            {
                if (e.NameHash == hash && e.Name == name)
                {
                    e.Value = Move(value);
                    Changed();
                    return *this;
                }
            }
            m_custom.PushBack(CustomEntry{String(name), hash, Move(value)});
            Changed();
            return *this;
        }
        [[nodiscard]] usize CustomCount() const noexcept { return m_custom.Size(); }
        [[nodiscard]] const CustomEntry& GetCustom(usize index) const { return m_custom[index]; }
        /// The custom property by hashed name, or null.
        [[nodiscard]] const StyleValue* FindCustom(u64 nameHash, StringView name) const
        {
            for (const CustomEntry& e : m_custom)
            {
                if (e.NameHash == nameHash && e.Name == name)
                {
                    return &e.Value;
                }
            }
            return nullptr;
        }

        /// Remove a property (releases any owned resource). No-op if not set.
        bool Remove(StyleProperty prop)
        {
            for (usize i = 0; i < m_properties.Size(); ++i)
            {
                if (m_properties[i].Prop == prop)
                {
                    m_properties.RemoveAt(i);
                    Changed();
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
        void Changed() noexcept
        {
            if (m_ownerVersion != nullptr)
            {
                ++*m_ownerVersion;
            }
        }

        void SetOverwrite(StyleProperty prop, StyleValue value)
        {
            Changed();
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
        Array<CustomEntry> m_custom;
        u32* m_ownerVersion = nullptr;
    };

    RTTI_DEFINE_OBJECT(StyleRule, "rtti::ui")
}
