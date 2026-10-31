// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.i18n - :plural_rule partition
//
// A language's plural rule, as data a string table carries: for each category it uses but
// `other`, a condition in CLDR's rule syntax for integer counts ("one: n = 1; few: n % 10 =
// 2..4 and n % 100 != 12..14"). Parsed once, when set (a rule that
// does not parse is refused with its position), then evaluated per count: the conditions in
// CLDR's category order, the first that holds chosen, else other.
//
// The syntax: a condition is relations joined by `and` (binding tighter) and `or`; a relation is
// an operand (`n`, or `i`, the same for integers) optionally `% <value>`, then `=` or `!=`, then a
// list of values and ranges (`2..4, 22..24`). Fractions (CLDR's v, f, t) are not supported.

module;
#include "Core/Prelude.h"

export module foundation.i18n:plural_rule;

import foundation.core;

using namespace foundation::core;

export namespace foundation::i18n
{
    /// CLDR's plural categories, in its order (the order rules are tried in).
    enum class PluralCategory : u8
    {
        Zero,
        One,
        Two,
        Few,
        Many,
        Other,
    };
    inline constexpr u32 kPluralCategoryCount = 6;

    /// The category's name as CLDR and the tables write it ("zero" ... "other").
    [[nodiscard]] inline StringView PluralCategoryName(PluralCategory category) noexcept
    {
        constexpr StringView kNames[kPluralCategoryCount] = {u8"zero", u8"one", u8"two", u8"few", u8"many", u8"other"};
        return kNames[static_cast<u32>(category)];
    }
    /// The category a name means; false for a name that is none.
    [[nodiscard]] inline bool PluralCategoryNamed(StringView name, PluralCategory& out) noexcept
    {
        for (u32 i = 0; i < kPluralCategoryCount; ++i)
            if (PluralCategoryName(static_cast<PluralCategory>(i)) == name)
            {
                out = static_cast<PluralCategory>(i);
                return true;
            }
        return false;
    }

    /// Why a rule did not parse: what, and where in its text.
    struct PluralRuleError
    {
        String Message;
        usize Position = 0;
    };

    class PluralRule
    {
    public:
        /// Parses a whole rule ("one: n = 1; few: ..."); categories may come in any order, `other`
        /// takes no condition and may be left out, an empty rule means everything is other. False,
        /// with `error`, when it does not parse; the rule is then unchanged.
        bool Parse(StringView source, PluralRuleError* error = nullptr)
        {
            Condition parsed[kPluralCategoryCount - 1];
            bool present[kPluralCategoryCount - 1] = {};
            Parser p{source, 0, {}, false};
            p.SkipSpace();
            while (!p.AtEnd() && !p.failed)
            {
                const usize nameAt = p.at;
                const StringView name = p.Word();
                PluralCategory category = PluralCategory::Other;
                if (name.Size() == 0 || !PluralCategoryNamed(name, category))
                    p.Fail(u8"a category name (zero, one, two, few, many, other)", nameAt);
                else if (category != PluralCategory::Other && present[static_cast<u32>(category)])
                    p.Fail(u8"a category given once", nameAt);
                p.Expect(u8':');
                if (p.failed)
                    break;
                if (category == PluralCategory::Other)
                {
                    p.SkipSpace(); // other holds whatever the rest does not; a condition there is ignored
                    while (!p.AtEnd() && p.Peek() != u8';')
                        ++p.at;
                }
                else
                {
                    parsed[static_cast<u32>(category)] = p.ParseCondition();
                    present[static_cast<u32>(category)] = true;
                }
                p.SkipSpace();
                if (!p.AtEnd() && !p.failed)
                {
                    p.Expect(u8';');
                    p.SkipSpace();
                }
            }
            if (p.failed)
            {
                if (error != nullptr)
                    *error = p.error;
                return false;
            }
            for (u32 i = 0; i + 1 < kPluralCategoryCount; ++i)
            {
                m_conditions[i] = Move(parsed[i]);
                m_present[i] = present[i];
            }
            m_source = String(source);
            return true;
        }

        /// The category of count `n` (a negative count counts as its magnitude).
        [[nodiscard]] PluralCategory CategoryOf(i64 n) const noexcept
        {
            const u64 value = n < 0 ? static_cast<u64>(-(n + 1)) + 1u : static_cast<u64>(n);
            for (u32 i = 0; i + 1 < kPluralCategoryCount; ++i)
                if (m_present[i] && Holds(m_conditions[i], value))
                    return static_cast<PluralCategory>(i);
            return PluralCategory::Other;
        }
        /// Whether the rule has a condition for `category` (other always counts as used).
        [[nodiscard]] bool Uses(PluralCategory category) const noexcept
        {
            return category == PluralCategory::Other || m_present[static_cast<u32>(category)];
        }
        /// The text it was parsed from.
        [[nodiscard]] StringView Source() const noexcept { return m_source.AsView(); }

    private:
        struct Range
        {
            u64 Low = 0;
            u64 High = 0;
        };
        struct Relation
        {
            u64 Modulo = 0; // 0: none
            bool Equal = true;
            Array<Range> Ranges;
        };
        // Relations joined by `and`, those joined by `or`.
        using Conjunction = Array<Relation>;
        using Condition = Array<Conjunction>;

        struct Parser
        {
            StringView text;
            usize at;
            PluralRuleError error;
            bool failed;

            [[nodiscard]] bool AtEnd() const noexcept { return at >= text.Size(); }
            [[nodiscard]] utf8char Peek() const noexcept { return AtEnd() ? utf8char{0} : text[at]; }
            void SkipSpace() noexcept
            {
                while (!AtEnd() && (text[at] == u8' ' || text[at] == u8'\t' || text[at] == u8'\n' || text[at] == u8'\r'))
                    ++at;
            }
            void Fail(const char8_t* expected, usize where)
            {
                if (failed)
                    return;
                failed = true;
                error.Message = String(u8"expected ");
                error.Message.Append(StringView(expected));
                error.Position = where;
            }
            void Expect(utf8char c)
            {
                SkipSpace();
                if (Peek() != c)
                {
                    const char8_t text1[2] = {c, 0};
                    Fail(text1, at);
                    return;
                }
                ++at;
            }
            StringView Word()
            {
                SkipSpace();
                const usize start = at;
                while (!AtEnd() && ((text[at] >= u8'a' && text[at] <= u8'z') || (text[at] >= u8'A' && text[at] <= u8'Z')))
                    ++at;
                return text.SubStr(start, at - start);
            }
            u64 Number()
            {
                SkipSpace();
                if (AtEnd() || text[at] < u8'0' || text[at] > u8'9')
                {
                    Fail(u8"a number", at);
                    return 0;
                }
                u64 value = 0;
                while (!AtEnd() && text[at] >= u8'0' && text[at] <= u8'9')
                    value = value * 10u + static_cast<u64>(text[at++] - u8'0');
                return value;
            }
            bool TryWord(const char8_t* word)
            {
                SkipSpace();
                const usize save = at;
                const StringView w = Word();
                if (w == StringView(word))
                    return true;
                at = save;
                return false;
            }
            Relation ParseRelation()
            {
                Relation r;
                const usize operandAt = (SkipSpace(), at);
                const StringView operand = Word();
                if (operand != StringView(u8"n") && operand != StringView(u8"i"))
                {
                    Fail(u8"the operand n or i", operandAt);
                    return r;
                }
                SkipSpace();
                if (Peek() == u8'%')
                {
                    ++at;
                    r.Modulo = Number();
                    if (!failed && r.Modulo == 0)
                        Fail(u8"a modulo above zero", at);
                }
                SkipSpace();
                if (Peek() == u8'!' && at + 1 < text.Size() && text[at + 1] == u8'=')
                {
                    r.Equal = false;
                    at += 2;
                }
                else if (Peek() == u8'=')
                    ++at;
                else
                {
                    Fail(u8"= or !=", at);
                    return r;
                }
                for (;;)
                {
                    Range range;
                    range.Low = range.High = Number();
                    SkipSpace();
                    if (!failed && Peek() == u8'.' && at + 1 < text.Size() && text[at + 1] == u8'.')
                    {
                        at += 2;
                        range.High = Number();
                        if (!failed && range.High < range.Low)
                            Fail(u8"a range from low to high", at);
                    }
                    if (failed)
                        return r;
                    r.Ranges.PushBack(range);
                    SkipSpace();
                    if (Peek() != u8',')
                        break;
                    ++at;
                }
                return r;
            }
            Condition ParseCondition()
            {
                Condition condition;
                for (;;)
                {
                    Conjunction conjunction;
                    conjunction.PushBack(ParseRelation());
                    while (!failed && TryWord(u8"and"))
                        conjunction.PushBack(ParseRelation());
                    if (failed)
                        return condition;
                    condition.PushBack(Move(conjunction));
                    if (!TryWord(u8"or"))
                        break;
                }
                SkipSpace();
                if (!failed && !AtEnd() && Peek() != u8';')
                    Fail(u8"and, or, ; or the end", at);
                return condition;
            }
        };

        [[nodiscard]] static bool Holds(const Condition& condition, u64 n) noexcept
        {
            for (const Conjunction& conjunction : condition)
            {
                bool all = true;
                for (const Relation& r : conjunction)
                {
                    const u64 value = r.Modulo != 0 ? n % r.Modulo : n;
                    bool inRanges = false;
                    for (const Range& range : r.Ranges)
                        inRanges = inRanges || (value >= range.Low && value <= range.High);
                    if (inRanges != r.Equal)
                    {
                        all = false;
                        break;
                    }
                }
                if (all)
                    return true;
            }
            return false;
        }

        Condition m_conditions[kPluralCategoryCount - 1];
        bool m_present[kPluralCategoryCount - 1] = {};
        String m_source;
    };
}
