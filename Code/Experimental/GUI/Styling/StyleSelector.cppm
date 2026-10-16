// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :style_selector partition
//
// CSS selector matching, ported from eepp's css/StyleSheetSelector(+Rule). A StyleSelector
// is a chain of compound StyleSelectorRules joined by combinators (descendant / child);
// each rule constrains tag / #id / .class / :pseudo. Matching keys off the UIWidget identity
// (tag/id/classes) from Phase 5, and pseudo-classes map onto the input-driven control state
// (:hover/:active/:focus/:focus-visible/:disabled -> IsHovered/IsPressed/IsFocused/IsFocusVisible/
// !IsEnabled).
//
// Supported subset (the common case): tag, #id, .class, :hover/:focus/:active/:disabled,
// universal '*', and descendant (space) / child ('>') combinators. Deferred: sibling
// combinators, structural pseudo (:nth-child), attribute selectors, :not.

module;
#include "Core/Prelude.h"

export module experimental.gui:style_selector;

import foundation.core; // String, StringView, Array, i64, Cast
import :node;
import :ui_widget;
import :parse_util; // IsIdentChar, ReadIdent

using namespace foundation::core;
namespace core = foundation::core;

namespace experimental::gui
{
    // CSS specificity buckets (packed, matching eepp: lexicographic id > class > tag).
    inline constexpr i64 kSpecificityId = 1048576;
    inline constexpr i64 kSpecificityClass = 1024;
    inline constexpr i64 kSpecificityTag = 1;
}

export namespace experimental::gui
{
    enum class Combinator
    {
        Descendant,
        Child
    };

    enum PseudoClass : u32
    {
        PseudoNone = 0,
        PseudoHover = 1u << 0,
        PseudoFocus = 1u << 1,
        PseudoActive = 1u << 2,
        PseudoDisabled = 1u << 3,
        PseudoFocusVisible = 1u << 4,
    };

    // A single compound selector (e.g. "button#ok.primary:hover") plus the combinator that
    // relates it to the rule on its left.
    class StyleSelectorRule
    {
    public:
        StyleSelectorRule() = default;
        StyleSelectorRule(core::StringView fragment, Combinator combinator)
            : m_combinator(combinator)
        {
            Parse(fragment);
        }

        [[nodiscard]] Combinator GetCombinator() const noexcept { return m_combinator; }
        [[nodiscard]] i64 Specificity() const noexcept { return m_specificity; }
        [[nodiscard]] core::StringView GetTag() const { return m_tag.AsView(); }
        [[nodiscard]] core::StringView GetId() const { return m_id.AsView(); }
        [[nodiscard]] u32 GetPseudoClasses() const noexcept { return m_pseudo; }
        // The `::part` pseudo-element (empty = none) - its declarations style a widget part.
        [[nodiscard]] core::StringView GetPseudoElement() const { return m_pseudoElement.AsView(); }

        [[nodiscard]] bool Matches(const UIWidget& element, bool applyPseudo = true) const
        {
            if (m_tag.AsView().Size() != 0 && m_tag != element.GetTag())
                return false;
            if (m_id.AsView().Size() != 0 && m_id != element.GetId())
                return false;
            for (const core::String& cls : m_classes)
                if (!element.HasClass(cls.AsView()))
                    return false;

            if (applyPseudo && m_pseudo != PseudoNone)
            {
                if ((m_pseudo & PseudoHover) && !element.IsHovered())
                    return false;
                if ((m_pseudo & PseudoActive) && !element.IsPressed())
                    return false;
                if ((m_pseudo & PseudoFocusVisible) && !element.IsFocusVisible())
                    return false;
                if ((m_pseudo & PseudoFocus) && !element.IsFocused())
                    return false;
                if ((m_pseudo & PseudoDisabled) && element.IsEnabled())
                    return false;
            }
            return true;
        }

    private:
        void Parse(core::StringView fragment)
        {
            usize i = 0;
            const usize n = fragment.Size();
            while (i < n)
            {
                const char8_t c = fragment[i];
                if (c == u8'#')
                {
                    ++i;
                    m_id = ReadIdent(fragment, i);
                }
                else if (c == u8'.')
                {
                    ++i;
                    m_classes.PushBack(core::String(ReadIdent(fragment, i)));
                }
                else if (c == u8':')
                {
                    if (i + 1 < n && fragment[i + 1] == u8':')
                    {
                        i += 2;
                        m_pseudoElement = core::String(ReadIdent(fragment, i));
                    } // ::part
                    else
                    {
                        ++i;
                        ApplyPseudo(ReadIdent(fragment, i));
                    } // :pseudo-class
                }
                else if (c == u8'*')
                {
                    ++i;
                } // universal: no tag constraint
                else if (IsIdentChar(c))
                {
                    m_tag = ReadIdent(fragment, i);
                }
                else
                {
                    ++i;
                } // skip anything unsupported
            }
            ComputeSpecificity();
        }

        void ApplyPseudo(core::StringView name)
        {
            if (name == core::StringView(u8"hover"))
                m_pseudo |= PseudoHover;
            else if (name == core::StringView(u8"focus"))
                m_pseudo |= PseudoFocus;
            else if (name == core::StringView(u8"focus-visible"))
                m_pseudo |= PseudoFocusVisible;
            else if (name == core::StringView(u8"active"))
                m_pseudo |= PseudoActive;
            else if (name == core::StringView(u8"disabled"))
                m_pseudo |= PseudoDisabled;
            // unknown pseudo-classes are ignored
        }

        void ComputeSpecificity() noexcept
        {
            i64 s = 0;
            if (m_id.AsView().Size() != 0)
                s += kSpecificityId;
            s += static_cast<i64>(m_classes.Size()) * kSpecificityClass;
            s += static_cast<i64>(PopCount(m_pseudo)) * kSpecificityClass;
            if (m_tag.AsView().Size() != 0)
                s += kSpecificityTag;
            if (m_pseudoElement.AsView().Size() != 0)
                s += kSpecificityTag; // pseudo-elements count as a type
            m_specificity = s;
        }

        [[nodiscard]] static u32 PopCount(u32 v) noexcept
        {
            u32 c = 0;
            while (v != 0)
            {
                c += (v & 1u);
                v >>= 1u;
            }
            return c;
        }

        core::String m_tag;
        core::String m_id;
        core::String m_pseudoElement;
        Array<core::String> m_classes;
        u32 m_pseudo = PseudoNone;
        Combinator m_combinator = Combinator::Descendant;
        i64 m_specificity = 0;
    };

    class StyleSelector
    {
    public:
        StyleSelector() = default;
        explicit StyleSelector(core::StringView selector) { Parse(selector); }

        [[nodiscard]] i64 Specificity() const noexcept { return m_specificity; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_rules.Size() == 0; }
        [[nodiscard]] usize RuleCount() const noexcept { return m_rules.Size(); }
        // The pseudo-element of the subject (rightmost) rule; empty for a normal selector.
        [[nodiscard]] core::StringView PseudoElement() const
        {
            return m_rules.Size() != 0 ? m_rules[m_rules.Size() - 1].GetPseudoElement()
                                       : core::StringView{};
        }

        // True if `element` matches this selector (the rightmost rule matches the element,
        // and each preceding rule matches an ancestor per its combinator).
        [[nodiscard]] bool Select(const UIWidget& element, bool applyPseudo = true) const
        {
            if (m_rules.Size() == 0)
                return false;

            usize i = m_rules.Size() - 1;
            if (!m_rules[i].Matches(element, applyPseudo))
                return false;

            const Node* current = &element;
            while (i > 0)
            {
                const Combinator combinator = m_rules[i].GetCombinator();
                --i;
                const StyleSelectorRule& rule = m_rules[i];
                if (combinator == Combinator::Child)
                {
                    const UIWidget* parent = AsWidget(current->GetParent());
                    if (parent == nullptr || !rule.Matches(*parent, applyPseudo))
                        return false;
                    current = parent;
                }
                else // Descendant: match any ancestor
                {
                    Node* ancestor = current->GetParent();
                    bool found = false;
                    while (ancestor != nullptr)
                    {
                        if (const UIWidget* w = AsWidget(ancestor))
                            if (rule.Matches(*w, applyPseudo))
                            {
                                current = w;
                                found = true;
                                break;
                            }
                        ancestor = ancestor->GetParent();
                    }
                    if (!found)
                        return false;
                }
            }
            return true;
        }

    private:
        [[nodiscard]] static const UIWidget* AsWidget(Node* n)
        {
            return n != nullptr ? core::Cast<UIWidget>(n) : nullptr;
        }

        void Parse(core::StringView selector)
        {
            usize i = 0;
            const usize n = selector.Size();
            Combinator combinator =
                Combinator::Descendant; // relates the next fragment to the previous
            while (i < n)
            {
                while (i < n && IsWhiteSpace(selector[i]))
                    ++i;
                if (i >= n)
                    break;
                if (selector[i] == u8'>')
                {
                    combinator = Combinator::Child;
                    ++i;
                    continue;
                }

                const usize start = i;
                while (i < n && !IsWhiteSpace(selector[i]) && selector[i] != u8'>')
                    ++i;
                m_rules.PushBack(StyleSelectorRule(selector.SubStr(start, i - start), combinator));
                combinator = Combinator::Descendant;
            }

            m_specificity = 0;
            for (const StyleSelectorRule& rule : m_rules)
                m_specificity += rule.Specificity();
        }

        Array<StyleSelectorRule> m_rules;
        i64 m_specificity = 0;
    };
}
