// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :style_selector partition
//
// CSS selector matching (started from eepp's css/StyleSheetSelector; E3 of
// Specs/gui-lab-evolution.md). A StyleSelector is a chain of compound selectors joined by
// combinators: descendant (space), child (>), adjacent sibling (+) and general sibling (~). A
// compound constrains the tag, #id, .classes, [attributes] and :pseudo-classes of one widget:
//
// - state: :hover, :active, :focus, :focus-visible, :focus-within, :disabled, :enabled, and the
//   widget's own (:checked, :indeterminate, :read-only, :read-write, :placeholder-shown, :selected), which a
//   widget reports through UIWidget::GetWidgetStates;
// - structure: :first-child, :last-child, :only-child, :first-of-type, :last-of-type, :empty,
//   :nth-child(), :nth-last-child(), :nth-of-type(), :nth-last-of-type() (an+b, odd, even);
// - logic: :not(), :is() and :where() over selector lists;
// - attributes: [name], [name=value], ^=, $=, *=, ~= and |=, read from the widget's id, classes
//   and reflected properties by their CSS names (`checked`, `placeholder`, `text`, ...).
//
// A pseudo-class the lab does not know makes its rule match nothing (eepp's rule), so a typo
// does not widen a rule. Matching backtracks through the combinators, as CSS's does.

module;
#include "Core/Prelude.h"

export module experimental.gui:style_selector;

import foundation.core; // String, StringView, Array, i64, Cast, reflection
import :node;
import :ui_widget;
import :parse_util; // IsIdentChar, ReadIdent
import :data_model; // ConvertVariant (an attribute's value as text)
import :style_value; // SplitTopLevel

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
    /// How a compound relates to the one on its left.
    enum class Combinator : u8
    {
        Descendant, ///< `a b`
        Child,      ///< `a > b`
        Adjacent,   ///< `a + b`: b right after a
        Sibling,    ///< `a ~ b`: b after a, among the same parent's children
    };

    /// The pseudo-classes a compound can require, as flags.
    /// The pseudo-classes a compound can require, as flags; those that follow a node's state are
    /// its Node::StyleState bits.
    enum PseudoClass : u32
    {
        PseudoNone = 0,
        PseudoHover = Node::StateHover,
        PseudoFocus = Node::StateFocus,
        PseudoActive = Node::StateActive,
        PseudoDisabled = Node::StateDisabled,
        PseudoFocusVisible = Node::StateFocusVisible,
        PseudoEnabled = Node::StateEnabled,
        PseudoFocusWithin = Node::StateFocusWithin,
        PseudoChecked = Node::StateChecked,
        PseudoIndeterminate = Node::StateIndeterminate,
        PseudoReadOnly = Node::StateReadOnly,
        PseudoReadWrite = Node::StateReadWrite,
        PseudoPlaceholderShown = Node::StatePlaceholderShown,
        PseudoEmpty = 1u << 12,
        PseudoFirstChild = 1u << 13,
        PseudoLastChild = 1u << 14,
        PseudoOnlyChild = 1u << 15,
        PseudoFirstOfType = 1u << 16,
        PseudoLastOfType = 1u << 17,
        PseudoSelected = Node::StateSelected,
    };
    /// The pseudo-classes that follow interaction (what `applyPseudo = false` ignores).
    inline constexpr u32 kDynamicPseudoClasses = PseudoHover | PseudoFocus | PseudoActive | PseudoDisabled | PseudoFocusVisible |
                                                 PseudoEnabled | PseudoFocusWithin | PseudoChecked | PseudoIndeterminate |
                                                 PseudoReadOnly | PseudoReadWrite | PseudoPlaceholderShown |
                                                 PseudoSelected;

    /// `:nth-child(an+b)` and its kin: the positions an+b (n = 0, 1, 2, ...), 1-based.
    struct NthPattern
    {
        i32 A = 0;
        i32 B = 1;
        bool OfType = false;  ///< count siblings of the same tag only
        bool FromEnd = false; ///< count from the last sibling
        [[nodiscard]] bool Matches(i32 position) const noexcept
        {
            if (A == 0)
                return position == B;
            const i32 steps = position - B;
            return steps % A == 0 && steps / A >= 0;
        }
    };

    /// `[name]`, `[name=value]` and the substring forms.
    struct AttributeTest
    {
        enum class Op : u8
        {
            Exists,
            Equals,   ///< =
            Prefix,   ///< ^=
            Suffix,   ///< $=
            Contains, ///< *=
            Word,     ///< ~= (one of its space-separated words)
            Dash,     ///< |= (equal, or followed by a dash)
        };
        core::String Name;
        Op Operation = Op::Exists;
        core::String Value;
    };

    class StyleSelector;

    // One compound selector (e.g. "button#ok.primary:hover") plus the combinator that relates
    // it to the compound on its left.
    class StyleSelectorRule
    {
    public:
        StyleSelectorRule() = default;
        StyleSelectorRule(core::StringView fragment, Combinator combinator);

        [[nodiscard]] Combinator GetCombinator() const noexcept { return m_combinator; }
        [[nodiscard]] i64 Specificity() const noexcept { return m_specificity; }
        [[nodiscard]] core::StringView GetTag() const { return m_tag.AsView(); }
        [[nodiscard]] core::StringView GetId() const { return m_id.AsView(); }
        [[nodiscard]] u32 GetPseudoClasses() const noexcept { return m_pseudo; }
        // The `::part` pseudo-element (empty = none) - its declarations style a widget part.
        [[nodiscard]] core::StringView GetPseudoElement() const { return m_pseudoElement.AsView(); }
        /// The part's own states it asks for (`::scrollbar-thumb:hover`): :hover and :active after
        /// the part, which the widget reports for it (GetPartStates).
        [[nodiscard]] u32 GetPartPseudoClasses() const noexcept { return m_partPseudo; }
        /// Whether it names a pseudo-class the lab does not know (it then matches nothing).
        [[nodiscard]] bool IsNever() const noexcept { return m_never; }
        /// Calls `f(selector)` for each selector in its :not(), :is() and :where().
        template <typename F>
        void ForEachNested(F&& f) const;

        [[nodiscard]] bool Matches(const UIWidget& element, bool applyPseudo = true) const;

    private:
        void Parse(core::StringView fragment);
        void ApplyPseudo(core::StringView name, core::StringView argument, bool functional);
        [[nodiscard]] bool MatchesPseudo(const UIWidget& element, bool applyPseudo) const;
        void ComputeSpecificity();

        core::String m_tag;
        core::String m_id;
        core::String m_pseudoElement;
        u32 m_partPseudo = 0; // pseudo-classes after the part: the part's states
        Array<core::String> m_classes;
        Array<AttributeTest> m_attributes;
        Array<NthPattern> m_nths;
        Array<StyleSelector> m_not;   // :not(list): matches none
        Array<StyleSelector> m_is;    // :is(list): matches one
        Array<StyleSelector> m_where; // :where(list): as :is, adding no specificity
        u32 m_pseudo = PseudoNone;
        bool m_never = false;
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
        [[nodiscard]] const StyleSelectorRule& RuleAt(usize i) const { return m_rules[i]; }
        // The pseudo-element of the subject (rightmost) rule; empty for a normal selector.
        [[nodiscard]] core::StringView PseudoElement() const
        {
            return m_rules.Size() != 0 ? m_rules[m_rules.Size() - 1].GetPseudoElement() : core::StringView{};
        }
        /// The part's own states the subject asks for (none for a normal selector).
        [[nodiscard]] u32 PartPseudoClasses() const noexcept
        {
            return m_rules.Size() != 0 ? m_rules[m_rules.Size() - 1].GetPartPseudoClasses() : 0u;
        }

        // True if `element` matches this selector: the rightmost compound matches it and each
        // one before matches a widget its combinator relates it to (trying every candidate).
        [[nodiscard]] bool Select(const UIWidget& element, bool applyPseudo = true) const
        {
            return m_rules.Size() != 0 && MatchAt(m_rules.Size() - 1, element, applyPseudo);
        }

        /// The states this selector reads through an ancestor (`:hover x`, `:focus-within > x`) and
        /// through an earlier sibling (`:checked + x`): a change of one restyles the node's subtree,
        /// or its later siblings. `outer` is where the whole selector sits (inside a compound that
        /// is itself an ancestor's or a sibling's, every state it reads counts there).
        void CollectRelationStates(u32& ancestor, u32& sibling, u32 outer = 0, bool outerIsSibling = false) const
        {
            for (usize i = 0; i < m_rules.Size(); ++i)
            {
                const StyleSelectorRule& rule = m_rules[i];
                const bool subject = i + 1 == m_rules.Size();
                const u32 states = rule.GetPseudoClasses() & kDynamicPseudoClasses;
                u32 nestedAncestor = 0, nestedSibling = 0;
                if (subject)
                {
                    if (outer != 0)
                        (outerIsSibling ? sibling : ancestor) |= states;
                    rule.ForEachNested([&](const StyleSelector& s) { s.CollectRelationStates(ancestor, sibling, outer, outerIsSibling); });
                    continue;
                }
                const Combinator next = m_rules[i + 1].GetCombinator();
                const bool viaSibling = next == Combinator::Adjacent || next == Combinator::Sibling;
                (viaSibling ? sibling : ancestor) |= states;
                rule.ForEachNested([&](const StyleSelector& s) { s.CollectRelationStates(nestedAncestor, nestedSibling, 1, viaSibling); });
                (viaSibling ? sibling : ancestor) |= nestedAncestor | nestedSibling | AllDynamic(rule);
            }
        }

        /// A comma-separated list split at its top level (commas inside parentheses, brackets or
        /// quotes stay).
        [[nodiscard]] static Array<core::StringView> SplitList(core::StringView list)
        {
            Array<core::StringView> out;
            usize start = 0;
            i32 depth = 0;
            char8_t quote = 0;
            for (usize i = 0; i <= list.Size(); ++i)
            {
                const char8_t c = i < list.Size() ? list[i] : u8',';
                if (quote != 0)
                {
                    if (c == quote)
                        quote = 0;
                    continue;
                }
                if (c == u8'"' || c == u8'\'')
                    quote = c;
                else if (c == u8'(' || c == u8'[')
                    ++depth;
                else if (c == u8')' || c == u8']')
                    --depth;
                else if (c == u8',' && depth <= 0)
                {
                    const core::StringView part = Trim(list.SubStr(start, i - start));
                    if (part.Size() != 0)
                        out.PushBack(part);
                    start = i + 1;
                }
            }
            return out;
        }

        // The widget a combinator steps to.
        [[nodiscard]] static const UIWidget* AsWidget(const Node* n) { return n != nullptr ? core::Cast<UIWidget>(n) : nullptr; }
        [[nodiscard]] static const UIWidget* PreviousSibling(const UIWidget& element)
        {
            const Node* parent = element.GetParent();
            if (parent == nullptr)
                return nullptr;
            usize i = parent->GetChildIndex(&element);
            while (i > 0 && i != Node::kInvalidIndex)
            {
                --i;
                if (const UIWidget* w = AsWidget(parent->GetChildAt(i)))
                    return w;
            }
            return nullptr;
        }

    private:
        // Every dynamic state a compound's nested selectors read, all positions.
        [[nodiscard]] static u32 AllDynamic(const StyleSelectorRule& rule)
        {
            u32 all = 0;
            rule.ForEachNested([&](const StyleSelector& s)
            {
                for (const StyleSelectorRule& r : s.m_rules)
                    all |= (r.GetPseudoClasses() & kDynamicPseudoClasses) | AllDynamic(r);
            });
            return all;
        }

        [[nodiscard]] bool MatchAt(usize i, const UIWidget& element, bool applyPseudo) const
        {
            if (!m_rules[i].Matches(element, applyPseudo))
                return false;
            if (i == 0)
                return true;
            switch (m_rules[i].GetCombinator())
            {
            case Combinator::Child:
            {
                const UIWidget* parent = AsWidget(element.GetParent());
                return parent != nullptr && MatchAt(i - 1, *parent, applyPseudo);
            }
            case Combinator::Descendant:
                for (const Node* n = element.GetParent(); n != nullptr; n = n->GetParent())
                    if (const UIWidget* w = AsWidget(n); w != nullptr && MatchAt(i - 1, *w, applyPseudo))
                        return true;
                return false;
            case Combinator::Adjacent:
            {
                const UIWidget* previous = PreviousSibling(element);
                return previous != nullptr && MatchAt(i - 1, *previous, applyPseudo);
            }
            case Combinator::Sibling:
                for (const UIWidget* s = PreviousSibling(element); s != nullptr; s = PreviousSibling(*s))
                    if (MatchAt(i - 1, *s, applyPseudo))
                        return true;
                return false;
            }
            return false;
        }

        void Parse(core::StringView selector)
        {
            usize i = 0;
            const usize n = selector.Size();
            Combinator combinator = Combinator::Descendant; // relates the next fragment to the previous
            while (i < n)
            {
                while (i < n && IsWhiteSpace(selector[i]))
                    ++i;
                if (i >= n)
                    break;
                const char8_t c = selector[i];
                if (c == u8'>' || c == u8'+' || c == u8'~')
                {
                    combinator = c == u8'>' ? Combinator::Child : c == u8'+' ? Combinator::Adjacent : Combinator::Sibling;
                    ++i;
                    continue;
                }
                // A compound runs to the next whitespace or combinator outside brackets and quotes.
                const usize start = i;
                i32 depth = 0;
                char8_t quote = 0;
                while (i < n)
                {
                    const char8_t d = selector[i];
                    if (quote != 0)
                    {
                        if (d == quote)
                            quote = 0;
                    }
                    else if (d == u8'"' || d == u8'\'')
                        quote = d;
                    else if (d == u8'(' || d == u8'[')
                        ++depth;
                    else if (d == u8')' || d == u8']')
                        --depth;
                    else if (depth <= 0 && (IsWhiteSpace(d) || d == u8'>' || d == u8'+' || d == u8'~'))
                        break;
                    ++i;
                }
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

    namespace detail
    {
        // `an+b`, `odd`, `even`, `n`, `-n+3`, `5`.
        [[nodiscard]] inline Optional<NthPattern> ParseNth(core::StringView text)
        {
            core::String compact;
            for (usize i = 0; i < text.Size(); ++i)
                if (!IsWhiteSpace(text[i]))
                    compact.Append(text.SubStr(i, 1));
            const core::StringView t = compact.AsView();
            if (t == core::StringView(u8"odd"))
                return NthPattern{2, 1};
            if (t == core::StringView(u8"even"))
                return NthPattern{2, 0};
            const auto integer = [](core::StringView s, i32& out) -> bool
            {
                if (s.Size() == 0)
                    return false;
                usize k = 0;
                bool negative = false;
                if (s[0] == u8'+' || s[0] == u8'-')
                {
                    negative = s[0] == u8'-';
                    ++k;
                }
                if (k == s.Size())
                    return false;
                i32 v = 0;
                for (; k < s.Size(); ++k)
                {
                    if (s[k] < u8'0' || s[k] > u8'9')
                        return false;
                    v = v * 10 + static_cast<i32>(s[k] - u8'0');
                }
                out = negative ? -v : v;
                return true;
            };
            usize nAt = Node::kInvalidIndex;
            for (usize i = 0; i < t.Size(); ++i)
                if (t[i] == u8'n' || t[i] == u8'N')
                    nAt = i;
            NthPattern out;
            if (nAt == Node::kInvalidIndex)
            {
                out.A = 0;
                if (!integer(t, out.B))
                    return {};
                return out;
            }
            const core::StringView a = t.SubStr(0, nAt);
            const core::StringView b = t.SubStr(nAt + 1, t.Size() - nAt - 1);
            if (a.Size() == 0 || a == core::StringView(u8"+"))
                out.A = 1;
            else if (a == core::StringView(u8"-"))
                out.A = -1;
            else if (!integer(a, out.A))
                return {};
            out.B = 0;
            if (b.Size() != 0 && !integer(b, out.B))
                return {};
            return out;
        }

        // The text of a widget's attribute: its id, its classes, or a reflected property by its
        // CSS name; none when it has no such attribute.
        [[nodiscard]] inline Optional<core::String> AttributeText(const UIWidget& element, core::StringView name)
        {
            if (name == core::StringView(u8"id"))
                return element.GetId().Size() != 0 ? Optional<core::String>(core::String(element.GetId())) : Optional<core::String>{};
            if (name == core::StringView(u8"class"))
            {
                core::String all;
                for (const core::String& c : element.Classes())
                {
                    if (all.Size() != 0)
                        all.Append(core::StringView(u8" "));
                    all.Append(c.AsView());
                }
                return all.Size() != 0 ? Optional<core::String>(core::Move(all)) : Optional<core::String>{};
            }
            for (const TypeInfo* type = element.GetType(); type != nullptr; type = type->base)
            {
                for (usize i = 0; i < PropertyCount(*type); ++i)
                {
                    const PropertyInfo& property = PropertyAt(*type, i);
                    const Attribute* css = FindAttribute(property, u8"css");
                    const core::String* cssName = css != nullptr ? css->value.TryGet<core::String>() : nullptr;
                    if (cssName == nullptr || cssName->AsView() != name)
                        continue;
                    const Variant value = GetProperty(property, Instance::From(const_cast<UIWidget*>(&element)));
                    const Variant text = ConvertVariant(value, TypeOf<core::String>());
                    if (const core::String* s = text.TryGet<core::String>())
                        return *s;
                    return {};
                }
            }
            return {};
        }

        [[nodiscard]] inline bool AttributeMatches(const AttributeTest& test, const UIWidget& element)
        {
            const Optional<core::String> text = AttributeText(element, test.Name.AsView());
            if (!text.HasValue())
                return false;
            const core::StringView v = text.Value().AsView();
            const core::StringView want = test.Value.AsView();
            switch (test.Operation)
            {
            case AttributeTest::Op::Exists: return true;
            case AttributeTest::Op::Equals: return v == want;
            case AttributeTest::Op::Prefix: return want.Size() != 0 && v.Size() >= want.Size() && v.SubStr(0, want.Size()) == want;
            case AttributeTest::Op::Suffix:
                return want.Size() != 0 && v.Size() >= want.Size() && v.SubStr(v.Size() - want.Size(), want.Size()) == want;
            case AttributeTest::Op::Contains:
                if (want.Size() == 0)
                    return false;
                for (usize i = 0; i + want.Size() <= v.Size(); ++i)
                    if (v.SubStr(i, want.Size()) == want)
                        return true;
                return false;
            case AttributeTest::Op::Word:
                for (const core::StringView word : SplitTopLevel(v, u8' '))
                    if (word == want)
                        return true;
                return false;
            case AttributeTest::Op::Dash:
                return v == want || (v.Size() > want.Size() && v.SubStr(0, want.Size()) == want && v[want.Size()] == u8'-');
            }
            return false;
        }

        // `[name op "value"]` (the brackets removed).
        [[nodiscard]] inline AttributeTest ParseAttribute(core::StringView inner)
        {
            AttributeTest test;
            usize i = 0;
            while (i < inner.Size() && IsWhiteSpace(inner[i]))
                ++i;
            const usize nameStart = i;
            while (i < inner.Size() && (IsIdentChar(inner[i])))
                ++i;
            test.Name = core::String(inner.SubStr(nameStart, i - nameStart));
            while (i < inner.Size() && IsWhiteSpace(inner[i]))
                ++i;
            if (i >= inner.Size())
                return test;
            using Op = AttributeTest::Op;
            const char8_t c = inner[i];
            if (c == u8'=')
            {
                test.Operation = Op::Equals;
                ++i;
            }
            else if (i + 1 < inner.Size() && inner[i + 1] == u8'=')
            {
                test.Operation = c == u8'^' ? Op::Prefix : c == u8'$' ? Op::Suffix : c == u8'*' ? Op::Contains : c == u8'~' ? Op::Word : Op::Dash;
                i += 2;
            }
            core::StringView value = Trim(inner.SubStr(i, inner.Size() - i));
            if (value.Size() >= 2 && (value[0] == u8'"' || value[0] == u8'\'') && value[value.Size() - 1] == value[0])
                value = value.SubStr(1, value.Size() - 2);
            test.Value = core::String(value);
            return test;
        }

        // The 1-based position of `element` among its parent's widgets (of its tag when
        // `ofType`), from the start or the end.
        [[nodiscard]] inline i32 PositionAmongSiblings(const UIWidget& element, bool ofType, bool fromEnd)
        {
            const Node* parent = element.GetParent();
            if (parent == nullptr)
                return 1;
            i32 position = 0;
            const usize count = parent->ChildCount();
            for (usize k = 0; k < count; ++k)
            {
                const usize i = fromEnd ? count - 1 - k : k;
                const UIWidget* w = StyleSelector::AsWidget(parent->GetChildAt(i));
                if (w == nullptr || (ofType && w->GetTag() != element.GetTag()))
                    continue;
                ++position;
                if (w == &element)
                    return position;
            }
            return position;
        }
    }

    template <typename F>
    inline void StyleSelectorRule::ForEachNested(F&& f) const
    {
        for (const StyleSelector& s : m_not)
            f(s);
        for (const StyleSelector& s : m_is)
            f(s);
        for (const StyleSelector& s : m_where)
            f(s);
    }

    inline StyleSelectorRule::StyleSelectorRule(core::StringView fragment, Combinator combinator)
        : m_combinator(combinator)
    {
        Parse(fragment);
    }

    inline bool StyleSelectorRule::Matches(const UIWidget& element, bool applyPseudo) const
    {
        if (m_never)
            return false;
        if (m_tag.AsView().Size() != 0 && m_tag != element.GetTag())
            return false;
        if (m_id.AsView().Size() != 0 && m_id != element.GetId())
            return false;
        for (const core::String& cls : m_classes)
            if (!element.HasClass(cls.AsView()))
                return false;
        for (const AttributeTest& test : m_attributes)
            if (!detail::AttributeMatches(test, element))
                return false;
        if (!MatchesPseudo(element, applyPseudo))
            return false;
        for (const NthPattern& nth : m_nths)
            if (!nth.Matches(detail::PositionAmongSiblings(element, nth.OfType, nth.FromEnd)))
                return false;
        for (const StyleSelector& s : m_not)
            if (s.Select(element, applyPseudo))
                return false;
        const auto any = [&](const Array<StyleSelector>& list)
        {
            for (const StyleSelector& s : list)
                if (s.Select(element, applyPseudo))
                    return true;
            return false;
        };
        if (!m_is.IsEmpty() && !any(m_is))
            return false;
        if (!m_where.IsEmpty() && !any(m_where))
            return false;
        return true;
    }

    inline bool StyleSelectorRule::MatchesPseudo(const UIWidget& element, bool applyPseudo) const
    {
        const u32 wanted = applyPseudo ? m_pseudo : (m_pseudo & ~kDynamicPseudoClasses);
        if (wanted == PseudoNone)
            return true;
        const u32 states = element.GetWidgetStates();
        const auto has = [&](u32 flag) { return (wanted & flag) != 0; };
        if (has(PseudoHover) && !element.IsHovered())
            return false;
        if (has(PseudoActive) && !element.IsPressed())
            return false;
        if (has(PseudoFocusVisible) && !element.IsFocusVisible())
            return false;
        if (has(PseudoFocus) && !element.IsFocused())
            return false;
        if (has(PseudoFocusWithin) && !element.IsFocusWithin())
            return false;
        if (has(PseudoDisabled) && element.IsEnabled())
            return false;
        if (has(PseudoEnabled) && !element.IsEnabled())
            return false;
        if (has(PseudoChecked) && (states & WidgetStateChecked) == 0)
            return false;
        if (has(PseudoIndeterminate) && (states & WidgetStateIndeterminate) == 0)
            return false;
        if (has(PseudoReadOnly) && (states & WidgetStateEditable) != 0)
            return false;
        if (has(PseudoReadWrite) && (states & WidgetStateEditable) == 0)
            return false;
        if (has(PseudoPlaceholderShown) && (states & WidgetStatePlaceholderShown) == 0)
            return false;
        if (has(PseudoSelected) && (states & WidgetStateSelected) == 0)
            return false;
        if (has(PseudoEmpty) && element.ChildCount() != 0)
            return false;
        const bool first = has(PseudoFirstChild) || has(PseudoOnlyChild);
        const bool last = has(PseudoLastChild) || has(PseudoOnlyChild);
        if (first && detail::PositionAmongSiblings(element, false, false) != 1)
            return false;
        if (last && detail::PositionAmongSiblings(element, false, true) != 1)
            return false;
        if (has(PseudoFirstOfType) && detail::PositionAmongSiblings(element, true, false) != 1)
            return false;
        if (has(PseudoLastOfType) && detail::PositionAmongSiblings(element, true, true) != 1)
            return false;
        return true;
    }

    inline void StyleSelectorRule::Parse(core::StringView fragment)
    {
        usize i = 0;
        const usize n = fragment.Size();
        // The text inside a bracket pair starting at `open` (i left after the closing one).
        const auto enclosed = [&](char8_t opening, char8_t closing) -> core::StringView
        {
            const usize start = i + 1;
            i32 depth = 0;
            char8_t quote = 0;
            for (; i < n; ++i)
            {
                const char8_t c = fragment[i];
                if (quote != 0)
                {
                    if (c == quote)
                        quote = 0;
                    continue;
                }
                if (c == u8'"' || c == u8'\'')
                    quote = c;
                else if (c == opening)
                    ++depth;
                else if (c == closing && --depth == 0)
                {
                    ++i;
                    return fragment.SubStr(start, i - 1 - start);
                }
            }
            m_never = true; // unclosed
            return fragment.SubStr(start, n > start ? n - start : 0);
        };
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
            else if (c == u8'[')
            {
                m_attributes.PushBack(detail::ParseAttribute(enclosed(u8'[', u8']')));
            }
            else if (c == u8':')
            {
                if (i + 1 < n && fragment[i + 1] == u8':')
                {
                    i += 2;
                    m_pseudoElement = core::String(ReadIdent(fragment, i));
                }
                else if (m_pseudoElement.AsView().Size() != 0)
                {
                    // After the part, a pseudo-class is the part's own state: :hover and :active,
                    // the states a part has (any other matches nothing).
                    ++i;
                    const core::StringView name = ReadIdent(fragment, i);
                    if (name == core::StringView(u8"hover"))
                        m_partPseudo |= PseudoHover;
                    else if (name == core::StringView(u8"active"))
                        m_partPseudo |= PseudoActive;
                    else
                        m_never = true;
                }
                else
                {
                    ++i;
                    const core::StringView name = ReadIdent(fragment, i);
                    if (i < n && fragment[i] == u8'(')
                        ApplyPseudo(name, enclosed(u8'(', u8')'), true);
                    else
                        ApplyPseudo(name, {}, false);
                }
            }
            else if (c == u8'*')
            {
                ++i; // universal: no tag constraint
            }
            else if (IsIdentChar(c))
            {
                m_tag = ReadIdent(fragment, i);
            }
            else
            {
                m_never = true; // a character no selector has
                ++i;
            }
        }
        ComputeSpecificity();
    }

    inline void StyleSelectorRule::ApplyPseudo(core::StringView name, core::StringView argument, bool functional)
    {
        if (functional)
        {
            const auto selectors = [&](Array<StyleSelector>& into)
            {
                for (const core::StringView part : StyleSelector::SplitList(argument))
                    into.PushBack(StyleSelector(part));
                if (into.IsEmpty())
                    m_never = true;
            };
            if (name == core::StringView(u8"not"))
                selectors(m_not);
            else if (name == core::StringView(u8"is") || name == core::StringView(u8"matches"))
                selectors(m_is);
            else if (name == core::StringView(u8"where"))
                selectors(m_where);
            else if (name == core::StringView(u8"nth-child") || name == core::StringView(u8"nth-last-child") ||
                     name == core::StringView(u8"nth-of-type") || name == core::StringView(u8"nth-last-of-type"))
            {
                Optional<NthPattern> nth = detail::ParseNth(argument);
                if (!nth.HasValue())
                {
                    m_never = true;
                    return;
                }
                nth.Value().OfType = name == core::StringView(u8"nth-of-type") || name == core::StringView(u8"nth-last-of-type");
                nth.Value().FromEnd = name == core::StringView(u8"nth-last-child") || name == core::StringView(u8"nth-last-of-type");
                m_nths.PushBack(nth.Value());
            }
            else
                m_never = true; // an unknown function
            return;
        }
        struct Named
        {
            const char8_t* Name;
            u32 Flag;
        };
        static constexpr Named kNames[] = {
            {u8"hover", PseudoHover},
            {u8"focus", PseudoFocus},
            {u8"focus-visible", PseudoFocusVisible},
            {u8"focus-within", PseudoFocusWithin},
            {u8"active", PseudoActive},
            {u8"disabled", PseudoDisabled},
            {u8"enabled", PseudoEnabled},
            {u8"checked", PseudoChecked},
            {u8"indeterminate", PseudoIndeterminate},
            {u8"read-only", PseudoReadOnly},
            {u8"read-write", PseudoReadWrite},
            {u8"placeholder-shown", PseudoPlaceholderShown},
            {u8"selected", PseudoSelected},
            {u8"empty", PseudoEmpty},
            {u8"first-child", PseudoFirstChild},
            {u8"last-child", PseudoLastChild},
            {u8"only-child", PseudoOnlyChild},
            {u8"first-of-type", PseudoFirstOfType},
            {u8"last-of-type", PseudoLastOfType},
        };
        for (const Named& named : kNames)
            if (name == core::StringView(named.Name))
            {
                m_pseudo |= named.Flag;
                return;
            }
        m_never = true; // an unknown pseudo-class matches nothing
    }

    inline void StyleSelectorRule::ComputeSpecificity()
    {
        i64 s = 0;
        if (m_id.AsView().Size() != 0)
            s += kSpecificityId;
        u32 pseudo = m_pseudo | (m_partPseudo << 16); // a part's states count as pseudo-classes too
        i64 classes = static_cast<i64>(m_classes.Size() + m_attributes.Size() + m_nths.Size());
        while (pseudo != 0)
        {
            classes += pseudo & 1u;
            pseudo >>= 1u;
        }
        s += classes * kSpecificityClass;
        if (m_tag.AsView().Size() != 0)
            s += kSpecificityTag;
        if (m_pseudoElement.AsView().Size() != 0)
            s += kSpecificityTag; // pseudo-elements count as a type
        // :not() and :is() add their most specific argument's; :where() adds nothing.
        const auto most = [](const Array<StyleSelector>& list)
        {
            i64 best = 0;
            for (const StyleSelector& sel : list)
                best = Max(best, sel.Specificity());
            return best;
        };
        s += most(m_not) + most(m_is);
        m_specificity = s;
    }
}
