// Draconic GUI - :style_selector partition
//
// CSS selector matching, ported from eepp's css/StyleSheetSelector(+Rule). A StyleSelector
// is a chain of compound StyleSelectorRules joined by combinators (descendant / child);
// each rule constrains tag / #id / .class / :pseudo. Matching keys off the UIWidget identity
// (tag/id/classes) from Phase 5, and pseudo-classes map onto the input-driven control state
// (:hover/:active/:focus/:disabled -> IsHovered/IsPressed/IsFocused/!IsEnabled).
//
// Supported subset (the common case): tag, #id, .class, :hover/:focus/:active/:disabled,
// universal '*', and descendant (space) / child ('>') combinators. Deferred: sibling
// combinators, structural pseudo (:nth-child), attribute selectors, :not.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:style_selector;

import draconic.foundation; // String, StringView, Array, i64, Cast
import :node;
import :ui_widget;
import :parse_util; // IsIdentChar, ReadIdent

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

namespace draconic::gui
{
    // CSS specificity buckets (packed, matching eepp: lexicographic id > class > tag).
    inline constexpr i64 kSpecificityId = 1048576;
    inline constexpr i64 kSpecificityClass = 1024;
    inline constexpr i64 kSpecificityTag = 1;
}

export namespace draconic::gui
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
    };

    // A single compound selector (e.g. "button#ok.primary:hover") plus the combinator that
    // relates it to the rule on its left.
    class StyleSelectorRule
    {
    public:
        StyleSelectorRule() = default;
        StyleSelectorRule(foundation::StringView fragment, Combinator combinator)
            : m_combinator(combinator)
        {
            Parse(fragment);
        }

        [[nodiscard]] Combinator GetCombinator() const noexcept { return m_combinator; }
        [[nodiscard]] i64 Specificity() const noexcept { return m_specificity; }
        [[nodiscard]] foundation::StringView GetTag() const { return m_tag.AsView(); }
        [[nodiscard]] foundation::StringView GetId() const { return m_id.AsView(); }
        [[nodiscard]] u32 GetPseudoClasses() const noexcept { return m_pseudo; }
        // The `::part` pseudo-element (empty = none) - its declarations style a widget part.
        [[nodiscard]] foundation::StringView GetPseudoElement() const { return m_pseudoElement.AsView(); }

        [[nodiscard]] bool Matches(const UIWidget& element, bool applyPseudo = true) const
        {
            if (m_tag.AsView().Size() != 0 && m_tag != element.GetTag())
                return false;
            if (m_id.AsView().Size() != 0 && m_id != element.GetId())
                return false;
            for (const foundation::String& cls : m_classes)
                if (!element.HasClass(cls.AsView()))
                    return false;

            if (applyPseudo && m_pseudo != PseudoNone)
            {
                if ((m_pseudo & PseudoHover) && !element.IsHovered())
                    return false;
                if ((m_pseudo & PseudoActive) && !element.IsPressed())
                    return false;
                if ((m_pseudo & PseudoFocus) && !element.IsFocused())
                    return false;
                if ((m_pseudo & PseudoDisabled) && element.IsEnabled())
                    return false;
            }
            return true;
        }

    private:
        void Parse(foundation::StringView fragment)
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
                    m_classes.PushBack(foundation::String(ReadIdent(fragment, i)));
                }
                else if (c == u8':')
                {
                    if (i + 1 < n && fragment[i + 1] == u8':')
                    {
                        i += 2;
                        m_pseudoElement = foundation::String(ReadIdent(fragment, i));
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

        void ApplyPseudo(foundation::StringView name)
        {
            if (name == foundation::StringView(u8"hover"))
                m_pseudo |= PseudoHover;
            else if (name == foundation::StringView(u8"focus"))
                m_pseudo |= PseudoFocus;
            else if (name == foundation::StringView(u8"active"))
                m_pseudo |= PseudoActive;
            else if (name == foundation::StringView(u8"disabled"))
                m_pseudo |= PseudoDisabled;
            // unknown pseudo-classes are ignored (deferred)
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

        foundation::String m_tag;
        foundation::String m_id;
        foundation::String m_pseudoElement;
        Array<foundation::String> m_classes;
        u32 m_pseudo = PseudoNone;
        Combinator m_combinator = Combinator::Descendant;
        i64 m_specificity = 0;
    };

    class StyleSelector
    {
    public:
        StyleSelector() = default;
        explicit StyleSelector(foundation::StringView selector) { Parse(selector); }

        [[nodiscard]] i64 Specificity() const noexcept { return m_specificity; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_rules.Size() == 0; }
        [[nodiscard]] usize RuleCount() const noexcept { return m_rules.Size(); }
        // The pseudo-element of the subject (rightmost) rule; empty for a normal selector.
        [[nodiscard]] foundation::StringView PseudoElement() const
        {
            return m_rules.Size() != 0 ? m_rules[m_rules.Size() - 1].GetPseudoElement()
                                       : foundation::StringView{};
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
            return n != nullptr ? foundation::Cast<UIWidget>(n) : nullptr;
        }

        void Parse(foundation::StringView selector)
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
