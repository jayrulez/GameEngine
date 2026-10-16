// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :style_sheet partition
//
// ResolvedStyle + StyleSheet: the cascade. Ported from eepp's css/StyleSheet(::getElement
// Styles). A StyleSheet is an ordered list of StyleRules; Resolve(element) gathers the rules
// that match, orders them by (specificity, source order), and applies their declarations so
// higher-specificity and later-source rules win - producing a flat name/value ResolvedStyle.

module;
#include "Core/Prelude.h"

export module experimental.gui:style_sheet;

import foundation.core; // String, StringView, Array, i64, Move
import :style_selector;
import :style_rule;
import :media_query;
import :ui_widget;
import :style_properties;
import :style_value; // SubstituteVariables

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    /// One property of a resolved style: its text (var() substituted) and its typed value.
    struct ResolvedProperty
    {
        core::String Name;
        core::String Value;
        StyleValue Typed; ///< Kind None: not a property the table parses, or its text did not parse.
        bool Important = false;
    };

    /// The cascaded properties of one element: longhands, var() substituted and parsed.
    class ResolvedStyle
    {
    public:
        /// Sets (or overrides) `name` as written; a shorthand or a var() is settled by Finalize. An
        /// !important value resists later normal ones.
        void Set(core::StringView name, core::StringView value, bool important = false)
        {
            Put(name, value, StyleValue{}, important);
            m_finalized = false;
        }

        /// Sets a longhand already settled (its text substituted and its value parsed).
        void SetParsed(core::StringView name, core::StringView value, const StyleValue& typed, bool important)
        {
            Put(name, value, typed, important);
        }

        [[nodiscard]] bool Has(core::StringView name) const { return Find(name) != nullptr; }

        /// The text of `name`, or `fallback`.
        [[nodiscard]] core::StringView Get(core::StringView name, core::StringView fallback = core::StringView{}) const
        {
            const ResolvedProperty* p = Find(name);
            return p != nullptr ? p->Value.AsView() : fallback;
        }

        /// The typed value of `name`, or null when it is unset or did not parse.
        [[nodiscard]] const StyleValue* Value(core::StringView name) const
        {
            const ResolvedProperty* p = Find(name);
            return p != nullptr && p->Typed.Kind != StyleValueKind::None ? &p->Typed : nullptr;
        }

        [[nodiscard]] usize Count() const noexcept { return m_props.Size(); }
        [[nodiscard]] const Array<ResolvedProperty>& Properties() const noexcept { return m_props; }
        [[nodiscard]] bool IsFinalized() const noexcept { return m_finalized; }

        /// Settles what Set left as written: substitutes var() anywhere in a value (its own custom
        /// properties first, then `inherited`'s), expands shorthands into longhands in order, and
        /// parses each longhand's value. Declarations whose var() cannot resolve are dropped.
        void Finalize(const ResolvedStyle* inherited = nullptr)
        {
            Array<ResolvedProperty> written = Move(m_props);
            m_props = Array<ResolvedProperty>{};
            for (ResolvedProperty& p : written)
                if (IsCustomProperty(p.Name.AsView()))
                    Put(p.Name.AsView(), p.Value.AsView(), StyleValue{}, p.Important);
            for (const ResolvedProperty& p : written)
            {
                if (IsCustomProperty(p.Name.AsView()))
                    continue;
                core::String text;
                if (!Substitute(p.Value.AsView(), inherited, text))
                    continue;
                SetDeclaration(p.Name.AsView(), text.AsView(), p.Important);
            }
            m_finalized = true;
        }

        /// Expands `name: text` (var() already substituted) into its longhands, parsed, and sets them.
        void SetDeclaration(core::StringView name, core::StringView text, bool important)
        {
            Array<StyleLonghand> longhands;
            if (!ExpandStyleShorthand(name, text, longhands))
                return;
            for (const StyleLonghand& longhand : longhands)
                SetParsed(longhand.Name.AsView(), longhand.Text.AsView(), ParseLonghand(longhand.Name.AsView(), longhand.Text.AsView()),
                          important);
        }

        /// A longhand's text parsed by the table's grammar for it (Kind None when the table does
        /// not name it, or it does not parse).
        [[nodiscard]] static StyleValue ParseLonghand(core::StringView name, core::StringView text)
        {
            StyleValue value;
            if (const StylePropertySpec* spec = FindStyleProperty(name))
                if (!ParseStyleValue(spec->Grammar, text, value))
                    value = StyleValue{};
            return value;
        }

        /// var() in `value` substituted from this style's custom properties, then `inherited`'s.
        [[nodiscard]] bool Substitute(core::StringView value, const ResolvedStyle* inherited, core::String& out) const
        {
            bool hasVar = false;
            for (usize i = 0; i + 3 < value.Size() && !hasVar; ++i)
                hasVar = value.SubStr(i, 4) == core::StringView(u8"var(");
            if (!hasVar)
            {
                out = core::String(value);
                return true;
            }
            const core::Function<Optional<core::String>(core::StringView)> lookup =
                [this, inherited](core::StringView name) -> Optional<core::String>
            {
                if (const ResolvedProperty* own = Find(name))
                    return core::String(own->Value);
                if (inherited != nullptr)
                    if (const ResolvedProperty* parent = inherited->Find(name))
                        return core::String(parent->Value);
                return {};
            };
            return SubstituteVariables(value, lookup, out);
        }

        /// This style's custom properties and `parent`'s it does not set (custom properties inherit).
        void InheritCustomProperties(const ResolvedStyle& parent)
        {
            for (const ResolvedProperty& p : parent.m_props)
                if (IsCustomProperty(p.Name.AsView()) && Find(p.Name.AsView()) == nullptr)
                    m_props.PushBack(p);
        }

        [[nodiscard]] static bool IsCustomProperty(core::StringView name) noexcept
        {
            return name.Size() >= 2 && name[0] == u8'-' && name[1] == u8'-';
        }

    private:
        [[nodiscard]] const ResolvedProperty* Find(core::StringView name) const
        {
            for (const ResolvedProperty& p : m_props)
                if (p.Name.AsView() == name)
                    return &p;
            return nullptr;
        }

        void Put(core::StringView name, core::StringView value, const StyleValue& typed, bool important)
        {
            for (ResolvedProperty& p : m_props)
            {
                if (p.Name.AsView() != name)
                    continue;
                if (p.Important && !important)
                    return; // important wins over normal
                p.Value = core::String(value);
                p.Typed = typed;
                p.Important = important;
                return;
            }
            m_props.PushBack(ResolvedProperty{core::String(name), core::String(value), typed, important});
        }

        Array<ResolvedProperty> m_props;
        bool m_finalized = true; // an empty style is settled
    };

    // One @keyframes stop: an offset in [0,1] (0% .. 100%, from/to) + its declarations.
    struct KeyframeStop
    {
        f32 Offset = 0.0f;
        Array<StyleProperty> Properties;
    };

    // A named @keyframes animation: an ordered list of stops.
    struct Keyframes
    {
        core::String Name;
        Array<KeyframeStop> Stops;
    };

    class StyleSheet
    {
    public:
        void AddRule(StyleRule rule) { m_rules.PushBack(core::Move(rule)); }
        [[nodiscard]] usize RuleCount() const noexcept { return m_rules.Size(); }
        [[nodiscard]] const Array<StyleRule>& Rules() const noexcept { return m_rules; }

        void AddKeyframes(Keyframes keyframes) { m_keyframes.PushBack(core::Move(keyframes)); }
        [[nodiscard]] usize KeyframesCount() const noexcept { return m_keyframes.Size(); }
        [[nodiscard]] const Keyframes* FindKeyframes(core::StringView name) const
        {
            for (const Keyframes& k : m_keyframes)
                if (k.Name.AsView() == name)
                    return &k;
            return nullptr;
        }

        // Resolve the cascade for `element`. With `pseudoElement` empty this is the element's
        // own style (rules with no ::part); pass a part name (e.g. "thumb") to resolve the
        // cascade for that widget part (rules written as `tag::part { ... }`).
        /// `inherited` (the parent's resolved style) gives the custom properties this element
        /// does not set.
        [[nodiscard]] ResolvedStyle Resolve(const UIWidget& element,
                                            const MediaContext& context = {},
                                            bool applyPseudo = true,
                                            core::StringView pseudoElement = {},
                                            const ResolvedStyle* inherited = nullptr) const
        {
            // Collect indices of matching rules (selector + pseudo-element + media), in source order.
            Array<usize> matches;
            for (usize i = 0; i < m_rules.Size(); ++i)
            {
                const StyleRule& rule = m_rules[i];
                if (rule.Selector().PseudoElement() != pseudoElement)
                    continue;
                const bool mediaActive = rule.Media().IsEmpty() || rule.Media().Evaluate(context);
                if (mediaActive && rule.Selector().Select(element, applyPseudo))
                    matches.PushBack(i);
            }

            // Stable insertion sort by specificity (ties keep source order -> later wins on apply).
            for (usize a = 1; a < matches.Size(); ++a)
            {
                const usize key = matches[a];
                const i64 keySpec = m_rules[key].Specificity();
                usize b = a;
                while (b > 0 && m_rules[matches[b - 1]].Specificity() > keySpec)
                {
                    matches[b] = matches[b - 1];
                    --b;
                }
                matches[b] = key;
            }

            // Apply low-to-high; later declarations override earlier (!important resists). Custom
            // properties first (this element's, then those it inherits), so a var() anywhere
            // resolves against the whole cascade; then each declaration as its longhands, parsed:
            // once per declaration when it has no var() (its value cannot change), else each time.
            ResolvedStyle out;
            for (const usize idx : matches)
                for (const StyleProperty& p : m_rules[idx].Properties())
                    if (ResolvedStyle::IsCustomProperty(p.Name.AsView()))
                        out.SetParsed(p.Name.AsView(), p.Value.AsView(), StyleValue{}, p.Important);
            if (inherited != nullptr)
                out.InheritCustomProperties(*inherited);
            for (const usize idx : matches)
            {
                for (const StyleProperty& p : m_rules[idx].Properties())
                {
                    if (ResolvedStyle::IsCustomProperty(p.Name.AsView()))
                        continue;
                    if (!p.Cached)
                        Cache(p);
                    if (p.CacheValid)
                    {
                        for (const StyleLonghandValue& longhand : p.Longhands)
                            out.SetParsed(longhand.Name.AsView(), longhand.Text.AsView(), longhand.Value, p.Important);
                        continue;
                    }
                    core::String text;
                    if (out.Substitute(p.Value.AsView(), nullptr, text))
                        out.SetDeclaration(p.Name.AsView(), text.AsView(), p.Important);
                }
            }
            return out;
        }

    private:
        // A declaration without var() expands and parses once: its longhands cached on it.
        static void Cache(const StyleProperty& p)
        {
            p.Cached = true;
            for (usize i = 0; i + 3 < p.Value.Size(); ++i)
                if (p.Value.AsView().SubStr(i, 4) == core::StringView(u8"var("))
                    return; // re-substituted each cascade
            Array<StyleLonghand> longhands;
            if (!ExpandStyleShorthand(p.Name.AsView(), p.Value.AsView(), longhands))
            {
                p.CacheValid = true; // a shorthand that does not parse sets nothing, every time
                return;
            }
            for (const StyleLonghand& longhand : longhands)
                p.Longhands.PushBack(StyleLonghandValue{longhand.Name, longhand.Text,
                                                        ResolvedStyle::ParseLonghand(longhand.Name.AsView(), longhand.Text.AsView())});
            p.CacheValid = true;
        }

        Array<StyleRule> m_rules;
        Array<Keyframes> m_keyframes;
    };
}
