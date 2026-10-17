// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.i18n - :localization partition
//
// Localization: the resolver its owner holds (Documentation/Specs/i18n.md). It borrows string
// tables (the last added wins for a key, and for a locale's rule or direction), holds the language
// and a fallback, and answers: a key's text through the chain (the language, its parents, the
// fallback, its parents), a plural key's form for a count by the rule of the language the text
// came from, positional formatting, and which way the language is written. A miss returns the key
// itself, logged once and kept for a tool to write out. Pseudolocalization transforms whatever it
// resolves. Owners hear of changes through a plain callback list (no bus: this stays a leaf).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h" // a missing key or rule, logged once

export module foundation.i18n:localization;

import foundation.core;
import :locale;
import :plural_rule;
import :string_table;

using namespace foundation::core;

export namespace foundation::i18n
{
    /// A span pseudolocalization leaves as it is, by its opening and closing marks.
    struct UntouchedSpan
    {
        String Open;
        String Close;
    };

    /// What pseudolocalization does to each text (Documentation/Specs/i18n.md).
    struct PseudoOptions
    {
        bool Enabled = false;
        bool Accents = true;        ///< letters accented ("Settings" to "Šéţţîñĝš"), still readable
        f32 Lengthen = 0.3f;        ///< padded with `~` by this share of its length (0: not at all)
        bool Brackets = true;       ///< each text between [ and ], so a cut-off end shows
        bool RightToLeft = false;   ///< wrapped in a right-to-left embedding, to try such a layout
        bool ShowKeys = false;      ///< each key beside its text ("menu.play: Play")
        /// Spans a consumer's markup needs untouched, each by its opening and closing marks
        /// ({"[", "]"} for rich-text tags, {"{{", "}}"} for bindings); `{0}` placeholders always are.
        Array<UntouchedSpan> Untouched;
    };

    /// A key asked for and found in no table, and the language it was asked in.
    struct MissingKey
    {
        String Key;
        String Language;
    };

    class Localization
    {
    public:
        // === Tables (borrowed) ===
        /// Adds a table, over those added before (it wins for its keys and locales).
        void AddTable(const StringTable* table)
        {
            if (table == nullptr)
                return;
            m_tables.PushBack(table);
            Changed();
        }
        void RemoveTable(const StringTable* table)
        {
            for (usize i = 0; i < m_tables.Size(); ++i)
                if (m_tables[i] == table)
                {
                    m_tables.RemoveAt(i);
                    Changed();
                    return;
                }
        }

        // === The language ===
        void SetLanguage(StringView locale)
        {
            m_language = String(locale);
            Changed();
        }
        [[nodiscard]] StringView Language() const noexcept { return m_language.AsView(); }
        void SetFallbackLanguage(StringView locale)
        {
            m_fallback = String(locale);
            Changed();
        }
        [[nodiscard]] StringView FallbackLanguage() const noexcept { return m_fallback.AsView(); }

        // === Texts ===
        /// Whether some table has the key in the chain's languages.
        [[nodiscard]] bool Has(StringView key) const { return Find(key).Text != nullptr; }
        /// The key's text in the chain (a plural key's `other` form), else the key itself, logged
        /// once. A view into a table (valid until that table changes or goes), or into this
        /// resolver's buffer while pseudolocalizing (valid until the next call).
        [[nodiscard]] StringView Text(StringView key)
        {
            const Found found = Find(key);
            if (found.Text == nullptr)
                return Missed(key);
            return Shown(key, found.Text->AsView());
        }
        /// Text, copied.
        [[nodiscard]] String TextString(StringView key) { return String(Text(key)); }

        /// The plural key's form for count `n` in the chain, by the plural rule of the language its
        /// text came from; its `other` form when that category is missing; the key when no table
        /// has it. A plain key gives its text.
        [[nodiscard]] StringView Plural(StringView key, i64 n)
        {
            const Found found = Find(key);
            if (found.Text == nullptr)
                return Missed(key);
            if (found.Table->IsPlural(key))
            {
                const PluralCategory category = PluralCategoryOf(found.Locale, n);
                if (const String* form = found.Table->PluralForm(key, found.Locale, category))
                    return Shown(key, form->AsView());
            }
            return Shown(key, found.Text->AsView());
        }

        /// The key's text with its placeholders `{0}`, `{1}` filled from `args` (in any order the
        /// translation puts them): bool, the integers, f32, f64, String and StringView print; `{{`
        /// and `}}` are braces; a placeholder naming no argument prints as written.
        [[nodiscard]] String Format(StringView key, Span<const Variant> args) { return Fill(Text(key), args); }
        /// Plural, then Format (`{0}` conventionally the count).
        [[nodiscard]] String FormatPlural(StringView key, i64 n, Span<const Variant> args) { return Fill(Plural(key, n), args); }

        // === What the tables say about languages ===
        /// The plural category of `n` in `locale`, by the last table's rule for it or its parents;
        /// Other for every count when no table gives one (logged once a locale).
        [[nodiscard]] PluralCategory PluralCategoryOf(StringView locale, i64 n)
        {
            for (StringView l = locale; l.Size() != 0; l = ParentLocale(l))
                for (usize t = m_tables.Size(); t-- > 0;)
                    if (const PluralRule* rule = m_tables[t]->GetPluralRule(l))
                        return rule->CategoryOf(n);
            const u64 hash = HashText(locale);
            if (!m_ruleless.Contains(hash))
            {
                m_ruleless.Insert(hash);
                LOG_WARNING(u8"I18N", u8"no plural rule for '{}': every count shows the other form", locale);
            }
            return PluralCategory::Other;
        }
        /// Whether the current language is written right to left (by the tables, through its
        /// parents; left to right when none says).
        [[nodiscard]] bool IsRightToLeft() const { return IsRightToLeft(m_language.AsView()); }
        [[nodiscard]] bool IsRightToLeft(StringView locale) const
        {
            for (StringView l = locale; l.Size() != 0; l = ParentLocale(l))
                for (usize t = m_tables.Size(); t-- > 0;)
                    if (const TextDirection d = m_tables[t]->GetDirection(l); d != TextDirection::Unset)
                        return d == TextDirection::RightToLeft;
            return false;
        }

        // === Missing keys ===
        [[nodiscard]] const Array<MissingKey>& MissingKeys() const noexcept { return m_missing; }
        void ClearMissingKeys()
        {
            m_missing.Clear();
            m_missedHashes = HashSet<u64>{};
        }

        // === Pseudolocalization ===
        void SetPseudolocalization(PseudoOptions options)
        {
            m_pseudo = Move(options);
            Changed();
        }
        [[nodiscard]] const PseudoOptions& GetPseudolocalization() const noexcept { return m_pseudo; }

        // === Changes ===
        /// Called when the language, the tables or pseudolocalization change; a token for removal.
        u32 OnChanged(Function<void()> callback)
        {
            m_callbacks.PushBack(Callback{++m_nextToken, Move(callback)});
            return m_nextToken;
        }
        void RemoveOnChanged(u32 token)
        {
            for (usize i = 0; i < m_callbacks.Size(); ++i)
                if (m_callbacks[i].Token == token)
                {
                    m_callbacks.RemoveAt(i);
                    return;
                }
        }

    private:
        struct Found
        {
            const String* Text = nullptr;
            const StringTable* Table = nullptr;
            StringView Locale;
        };
        struct Callback
        {
            u32 Token;
            Function<void()> Call;
        };

        // The first text along the chain: the language and its parents, then the fallback and its
        // parents; at each, the last table first.
        [[nodiscard]] Found Find(StringView key) const
        {
            const StringView starts[2] = {m_language.AsView(), m_fallback.AsView()};
            for (const StringView start : starts)
                for (StringView l = start; l.Size() != 0; l = ParentLocale(l))
                    for (usize t = m_tables.Size(); t-- > 0;)
                        if (const String* text = m_tables[t]->Text(key, l))
                            return Found{text, m_tables[t], l};
            return {};
        }

        StringView Missed(StringView key)
        {
            const u64 hash = HashText(key);
            if (!m_missedHashes.Contains(hash))
            {
                m_missedHashes.Insert(hash);
                m_missing.PushBack(MissingKey{String(key), m_language});
                LOG_WARNING(u8"I18N", u8"no text for '{}' in '{}'", key, m_language.AsView());
            }
            return Shown(key, key);
        }

        // The text as shown: itself, or pseudolocalized into the buffer.
        StringView Shown(StringView key, StringView text)
        {
            if (!m_pseudo.Enabled)
                return text;
            m_buffer = Pseudolocalize(key, text);
            return m_buffer.AsView();
        }

        [[nodiscard]] String Pseudolocalize(StringView key, StringView text) const
        {
            String out;
            if (m_pseudo.RightToLeft)
                out.Append(StringView(u8"‫"));
            if (m_pseudo.Brackets)
                out.Append(StringView(u8"["));
            if (m_pseudo.ShowKeys)
            {
                out.Append(key);
                out.Append(StringView(u8": "));
            }
            usize letters = 0;
            for (usize i = 0; i < text.Size();)
            {
                // A span to leave as it is: a placeholder, or one the consumer named.
                if (const usize kept = UntouchedAt(text, i); kept != 0)
                {
                    out.Append(text.SubStr(i, kept));
                    i += kept;
                    continue;
                }
                const utf8char c = text[i];
                const StringView accented = m_pseudo.Accents ? Accented(c) : StringView{};
                if (accented.Size() != 0)
                    out.Append(accented);
                else
                    out.Append(text.SubStr(i, 1));
                if ((c & 0xC0u) != 0x80u) // a code point's first byte: one letter
                    ++letters;
                ++i;
            }
            const usize pad = static_cast<usize>(static_cast<f32>(letters) * Max(0.0f, m_pseudo.Lengthen) + 0.5f);
            for (usize i = 0; i < pad; ++i)
                out.Append(StringView(u8"~"));
            if (m_pseudo.Brackets)
                out.Append(StringView(u8"]"));
            if (m_pseudo.RightToLeft)
                out.Append(StringView(u8"‬"));
            return out;
        }
        // The length of the untouched span starting at `at`, or 0.
        [[nodiscard]] usize UntouchedAt(StringView text, usize at) const noexcept
        {
            if (text[at] == u8'{')
            {
                usize end = at + 1;
                while (end < text.Size() && text[end] >= u8'0' && text[end] <= u8'9')
                    ++end;
                if (end > at + 1 && end < text.Size() && text[end] == u8'}')
                    return end + 1 - at;
            }
            for (const UntouchedSpan& marks : m_pseudo.Untouched)
            {
                const StringView open = marks.Open.AsView();
                const StringView close = marks.Close.AsView();
                if (open.Size() == 0 || at + open.Size() > text.Size() || text.SubStr(at, open.Size()) != open)
                    continue;
                for (usize end = at + open.Size(); end + close.Size() <= text.Size(); ++end)
                    if (text.SubStr(end, close.Size()) == close)
                        return end + close.Size() - at;
            }
            return 0;
        }
        // An ASCII letter's accented look-alike (empty for anything else).
        [[nodiscard]] static StringView Accented(utf8char c) noexcept
        {
            static constexpr const char8_t* kLower[26] = {u8"á", u8"ƀ", u8"ç", u8"ð", u8"é", u8"ƒ", u8"ĝ", u8"ĥ", u8"î", u8"ĵ", u8"ķ", u8"ļ", u8"ɱ",
                                                          u8"ñ", u8"ö", u8"þ", u8"ǫ", u8"ŕ", u8"š", u8"ţ", u8"û", u8"ṽ", u8"ŵ", u8"ẋ", u8"ý", u8"ž"};
            static constexpr const char8_t* kUpper[26] = {u8"Å", u8"Ɓ", u8"Ç", u8"Ð", u8"É", u8"Ƒ", u8"Ĝ", u8"Ĥ", u8"Î", u8"Ĵ", u8"Ķ", u8"Ļ", u8"Ṁ",
                                                          u8"Ñ", u8"Ö", u8"Þ", u8"Ǫ", u8"Ŕ", u8"Š", u8"Ţ", u8"Û", u8"Ṽ", u8"Ŵ", u8"Ẋ", u8"Ý", u8"Ž"};
            if (c >= u8'a' && c <= u8'z')
                return StringView(kLower[c - u8'a']);
            if (c >= u8'A' && c <= u8'Z')
                return StringView(kUpper[c - u8'A']);
            return {};
        }

        // Positional placeholders filled.
        [[nodiscard]] static String Fill(StringView text, Span<const Variant> args)
        {
            String out;
            for (usize i = 0; i < text.Size();)
            {
                const utf8char c = text[i];
                if ((c == u8'{' || c == u8'}') && i + 1 < text.Size() && text[i + 1] == c)
                {
                    out.Append(text.SubStr(i, 1)); // an escaped brace
                    i += 2;
                    continue;
                }
                if (c == u8'{')
                {
                    usize end = i + 1;
                    usize index = 0;
                    while (end < text.Size() && text[end] >= u8'0' && text[end] <= u8'9')
                        index = index * 10u + static_cast<usize>(text[end++] - u8'0');
                    if (end > i + 1 && end < text.Size() && text[end] == u8'}' && index < args.Size())
                    {
                        out.Append(ArgumentText(args[index]).AsView());
                        i = end + 1;
                        continue;
                    }
                }
                out.Append(text.SubStr(i, 1)); // a placeholder naming no argument stays as written
                ++i;
            }
            return out;
        }
        [[nodiscard]] static String ArgumentText(const Variant& value)
        {
            if (const bool* b = value.TryGet<bool>())
                return String(*b ? StringView(u8"true") : StringView(u8"false"));
            if (const i32* v = value.TryGet<i32>())
                return foundation::core::Format(u8"{}", *v);
            if (const i64* v = value.TryGet<i64>())
                return foundation::core::Format(u8"{}", *v);
            if (const u32* v = value.TryGet<u32>())
                return foundation::core::Format(u8"{}", *v);
            if (const u64* v = value.TryGet<u64>())
                return foundation::core::Format(u8"{}", *v);
            if (const f32* v = value.TryGet<f32>())
                return foundation::core::Format(u8"{}", *v);
            if (const f64* v = value.TryGet<f64>())
                return foundation::core::Format(u8"{}", *v);
            if (const String* s = value.TryGet<String>())
                return *s;
            if (const StringView* s = value.TryGet<StringView>())
                return String(*s);
            return {};
        }

        void Changed()
        {
            for (usize i = 0; i < m_callbacks.Size(); ++i)
                m_callbacks[i].Call();
        }

        Array<const StringTable*> m_tables; // borrowed, the last winning
        String m_language{StringView(u8"en")};
        String m_fallback{StringView(u8"en")};
        Array<MissingKey> m_missing;
        HashSet<u64> m_missedHashes;
        HashSet<u64> m_ruleless; // locales already logged as having no rule
        PseudoOptions m_pseudo;
        String m_buffer;
        Array<Callback> m_callbacks;
        u32 m_nextToken = 0;
    };
}
