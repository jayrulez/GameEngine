// Draconic GUI - :style_sheet partition
//
// ResolvedStyle + StyleSheet: the cascade. Ported from eepp's css/StyleSheet(::getElement
// Styles). A StyleSheet is an ordered list of StyleRules; Resolve(element) gathers the rules
// that match, orders them by (specificity, source order), and applies their declarations so
// higher-specificity and later-source rules win - producing a flat name/value ResolvedStyle.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:style_sheet;

import draconic.foundation; // String, StringView, Array, i64, Move
import :style_selector;
import :style_rule;
import :media_query;
import :ui_widget;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    // The flat, cascaded property set for one element.
    class ResolvedStyle
    {
    public:
        // Set/override a property. An !important value resists later non-important overrides.
        void Set(foundation::StringView name, foundation::StringView value, bool important = false)
        {
            for (StyleProperty& p : m_props)
                if (p.Name == name)
                {
                    if (p.Important && !important)
                        return; // important wins over normal
                    p.Value = value;
                    p.Important = important;
                    return;
                }
            m_props.PushBack(StyleProperty(name, value, important));
        }

        [[nodiscard]] bool Has(foundation::StringView name) const
        {
            for (const StyleProperty& p : m_props)
                if (p.Name == name)
                    return true;
            return false;
        }

        [[nodiscard]] foundation::StringView Get(foundation::StringView name,
                                           foundation::StringView fallback = foundation::StringView{}) const
        {
            for (const StyleProperty& p : m_props)
                if (p.Name == name)
                    return p.Value.AsView();
            return fallback;
        }

        [[nodiscard]] usize Count() const noexcept { return m_props.Size(); }
        [[nodiscard]] const Array<StyleProperty>& Properties() const noexcept { return m_props; }

        // Substitute whole-value var(--name[, fallback]) references against the resolved
        // custom properties (one level; nested/partial var() deferred).
        void ResolveVariables()
        {
            for (StyleProperty& p : m_props)
            {
                if (IsCustomProperty(p.Name.AsView()))
                    continue;
                const foundation::StringView v = foundation::Trim(p.Value.AsView());
                if (v.Size() < 5 || v.SubStr(0, 4) != foundation::StringView(u8"var(") ||
                    v[v.Size() - 1] != u8')')
                    continue;

                const foundation::StringView inside = v.SubStr(4, v.Size() - 5);
                usize comma = inside.Size();
                for (usize i = 0; i < inside.Size(); ++i)
                    if (inside[i] == u8',')
                    {
                        comma = i;
                        break;
                    }

                const foundation::StringView varName = foundation::Trim(inside.SubStr(0, comma));
                const foundation::StringView fallback =
                    (comma < inside.Size())
                        ? foundation::Trim(inside.SubStr(comma + 1, inside.Size() - comma - 1))
                        : foundation::StringView{};
                p.Value =
                    Get(varName, fallback); // Get scans a different element; safe to assign here
            }
        }

    private:
        [[nodiscard]] static bool IsCustomProperty(foundation::StringView name) noexcept
        {
            return name.Size() >= 2 && name[0] == u8'-' && name[1] == u8'-';
        }

        Array<StyleProperty> m_props;
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
        foundation::String Name;
        Array<KeyframeStop> Stops;
    };

    class StyleSheet
    {
    public:
        void AddRule(StyleRule rule) { m_rules.PushBack(foundation::Move(rule)); }
        [[nodiscard]] usize RuleCount() const noexcept { return m_rules.Size(); }
        [[nodiscard]] const Array<StyleRule>& Rules() const noexcept { return m_rules; }

        void AddKeyframes(Keyframes keyframes) { m_keyframes.PushBack(foundation::Move(keyframes)); }
        [[nodiscard]] usize KeyframesCount() const noexcept { return m_keyframes.Size(); }
        [[nodiscard]] const Keyframes* FindKeyframes(foundation::StringView name) const
        {
            for (const Keyframes& k : m_keyframes)
                if (k.Name.AsView() == name)
                    return &k;
            return nullptr;
        }

        // Resolve the cascade for `element`. With `pseudoElement` empty this is the element's
        // own style (rules with no ::part); pass a part name (e.g. "thumb") to resolve the
        // cascade for that widget part (rules written as `tag::part { ... }`).
        [[nodiscard]] ResolvedStyle Resolve(const UIWidget& element,
                                            const MediaContext& context = {},
                                            bool applyPseudo = true,
                                            foundation::StringView pseudoElement = {}) const
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

            // Apply low-to-high; later Set() overrides earlier (!important resists).
            ResolvedStyle out;
            for (const usize idx : matches)
                for (const StyleProperty& p : m_rules[idx].Properties())
                    out.Set(p.Name.AsView(), p.Value.AsView(), p.Important);
            out.ResolveVariables();
            return out;
        }

    private:
        Array<StyleRule> m_rules;
        Array<Keyframes> m_keyframes;
    };
}
