// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.i18n - :string_table partition
//
// StringTable: the authored unit, one resource holding everything about its languages
// (Documentation/Specs/i18n.md). Locales, each with its plural rule and its direction (either may
// be unset, for a table that only adds texts to a locale another table describes); keys, each
// plain or plural in all its locales, a cell per locale missing, a text, or a plural entry (a text
// per category, `other` among them). Mutable, so an editor's grid and an import change it in
// place; serializable as a versioned payload, the same body an asset's source and its cooked
// product. Other formats are converted to it in the pipeline layer.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.i18n:string_table;

import foundation.core;
import :locale;
import :plural_rule;

using namespace foundation::core;

export namespace foundation::i18n
{
    /// A locale's direction as a table says it, or nothing said.
    enum class TextDirection : u8
    {
        Unset,
        LeftToRight,
        RightToLeft,
    };

    class StringTable final : public ISerializable
    {
        RTTI_OBJECT(StringTable, ISerializable)
    public:
        // === Locales ===
        /// Adds a locale (its id as given; one with a locale's shape); false when it is not shaped
        /// as one or the table has it.
        bool AddLocale(StringView locale)
        {
            if (!IsLocaleShaped(locale) || FindLocale(locale) != kNone)
                return false;
            m_locales.PushBack(LocaleInfo{String(locale), {}, false, TextDirection::Unset});
            for (Entry& e : m_entries)
                e.Cells.PushBack(Cell{});
            return true;
        }
        /// Removes a locale and every text in it; false when the table has none such.
        bool RemoveLocale(StringView locale)
        {
            const usize at = FindLocale(locale);
            if (at == kNone)
                return false;
            m_locales.RemoveAt(at);
            for (Entry& e : m_entries)
                e.Cells.RemoveAt(at);
            return true;
        }
        [[nodiscard]] usize LocaleCount() const noexcept { return m_locales.Size(); }
        [[nodiscard]] StringView LocaleAt(usize index) const noexcept { return m_locales[index].Id.AsView(); }
        [[nodiscard]] bool HasLocale(StringView locale) const noexcept { return FindLocale(locale) != kNone; }

        /// Sets the locale's plural rule ("one: n = 1; few: ..."); false, with `error`, when the
        /// locale is not the table's or the rule does not parse (the old rule then stays).
        bool SetPluralRule(StringView locale, StringView rule, PluralRuleError* error = nullptr)
        {
            const usize at = FindLocale(locale);
            if (at == kNone)
            {
                if (error != nullptr)
                    *error = PluralRuleError{String(u8"no such locale in the table"), 0};
                return false;
            }
            PluralRule parsed;
            if (!parsed.Parse(rule, error))
                return false;
            m_locales[at].Rule = Move(parsed);
            m_locales[at].HasRule = true;
            return true;
        }
        /// Leaves the locale's rule unset (another table may give it).
        void ClearPluralRule(StringView locale)
        {
            if (const usize at = FindLocale(locale); at != kNone)
                m_locales[at].HasRule = false;
        }
        /// The locale's rule, or null when this table sets none.
        [[nodiscard]] const PluralRule* GetPluralRule(StringView locale) const noexcept
        {
            const usize at = FindLocale(locale);
            return at != kNone && m_locales[at].HasRule ? &m_locales[at].Rule : nullptr;
        }
        /// Says which way the locale is written (Unset: this table does not say).
        void SetDirection(StringView locale, TextDirection direction)
        {
            if (const usize at = FindLocale(locale); at != kNone)
                m_locales[at].Direction = direction;
        }
        [[nodiscard]] TextDirection GetDirection(StringView locale) const noexcept
        {
            const usize at = FindLocale(locale);
            return at != kNone ? m_locales[at].Direction : TextDirection::Unset;
        }

        // === Keys and their texts ===
        /// Sets a plain key's text in a locale (the locale added when the table lacks it); false
        /// when the key is plural, or the locale has no locale's shape.
        bool Set(StringView key, StringView locale, StringView text)
        {
            const usize column = EnsureLocale(locale);
            if (column == kNone)
                return false;
            Entry& e = EnsureEntry(key, false);
            if (e.Plural)
                return false;
            Cell& cell = e.Cells[column];
            cell.Present = true;
            cell.Text = String(text);
            return true;
        }
        /// Sets one form of a plural key in a locale; false when the key is plain, or the locale
        /// has no locale's shape.
        bool SetPlural(StringView key, StringView locale, PluralCategory category, StringView text)
        {
            const usize column = EnsureLocale(locale);
            if (column == kNone)
                return false;
            Entry& e = EnsureEntry(key, true);
            if (!e.Plural)
                return false;
            Cell& cell = e.Cells[column];
            cell.Forms[static_cast<u32>(category)] = String(text);
            cell.FormsPresent = static_cast<u8>(cell.FormsPresent | (1u << static_cast<u32>(category)));
            return true;
        }
        /// Removes a key (in every locale); false when the table has none such.
        bool Remove(StringView key)
        {
            const usize at = FindEntry(key);
            if (at == kNone)
                return false;
            m_entries.RemoveAt(at);
            Reindex();
            return true;
        }
        /// Renames a key, its texts (every form) with it; false when `from` is none or `to` is
        /// taken.
        bool RenameKey(StringView from, StringView to)
        {
            const usize at = FindEntry(from);
            if (at == kNone || FindEntry(to) != kNone)
                return false;
            m_entries[at].Key = String(to);
            Reindex();
            return true;
        }
        /// Removes a key's text in one locale (the cell missing again).
        void Clear(StringView key, StringView locale)
        {
            const usize at = FindEntry(key);
            const usize column = FindLocale(locale);
            if (at != kNone && column != kNone)
                m_entries[at].Cells[column] = Cell{};
        }

        [[nodiscard]] usize KeyCount() const noexcept { return m_entries.Size(); }
        [[nodiscard]] StringView KeyAt(usize index) const noexcept { return m_entries[index].Key.AsView(); }
        [[nodiscard]] bool HasKey(StringView key) const noexcept { return FindEntry(key) != kNone; }
        [[nodiscard]] bool IsPlural(StringView key) const noexcept
        {
            const usize at = FindEntry(key);
            return at != kNone && m_entries[at].Plural;
        }
        /// Whether the key has a text in the locale (a plain text, or any plural form).
        [[nodiscard]] bool HasText(StringView key, StringView locale) const noexcept
        {
            const Cell* cell = FindCell(key, locale);
            return cell != nullptr && (cell->Present || cell->FormsPresent != 0);
        }
        /// The key's text in the locale: a plain key's, a plural key's `other` form; null when
        /// missing.
        [[nodiscard]] const String* Text(StringView key, StringView locale) const noexcept
        {
            const Cell* cell = FindCell(key, locale);
            if (cell == nullptr)
                return nullptr;
            if (cell->Present)
                return &cell->Text;
            return Form(*cell, PluralCategory::Other);
        }
        /// One form of a plural key in the locale; null when missing (or the key is plain).
        [[nodiscard]] const String* PluralForm(StringView key, StringView locale, PluralCategory category) const noexcept
        {
            const Cell* cell = FindCell(key, locale);
            return cell != nullptr ? Form(*cell, category) : nullptr;
        }
        /// The categories a plural key's entry in the locale lacks that `rule` uses (`other` always
        /// among those it needs); every used one when the entry is missing.
        [[nodiscard]] Array<PluralCategory> MissingForms(StringView key, StringView locale, const PluralRule* rule) const
        {
            Array<PluralCategory> out;
            const Cell* cell = FindCell(key, locale);
            for (u32 i = 0; i < kPluralCategoryCount; ++i)
            {
                const PluralCategory category = static_cast<PluralCategory>(i);
                const bool used = category == PluralCategory::Other || (rule != nullptr && rule->Uses(category));
                if (used && (cell == nullptr || (cell->FormsPresent & (1u << i)) == 0))
                    out.PushBack(category);
            }
            return out;
        }

        void Serialize(ISerializer& ar) override; // StringTableImpl.cpp

    private:
        static constexpr usize kNone = static_cast<usize>(-1);
        struct LocaleInfo
        {
            String Id;
            PluralRule Rule;
            bool HasRule = false;
            TextDirection Direction = TextDirection::Unset;
        };
        struct Cell
        {
            bool Present = false; // a plain text
            String Text;
            u8 FormsPresent = 0;  // a plural entry's forms, one bit a category
            String Forms[kPluralCategoryCount];
        };
        struct Entry
        {
            String Key;
            bool Plural = false;
            Array<Cell> Cells; // one a locale, in the locales' order
        };

        [[nodiscard]] static const String* Form(const Cell& cell, PluralCategory category) noexcept
        {
            const u32 bit = 1u << static_cast<u32>(category);
            return (cell.FormsPresent & bit) != 0 ? &cell.Forms[static_cast<u32>(category)] : nullptr;
        }
        [[nodiscard]] usize FindLocale(StringView locale) const noexcept
        {
            for (usize i = 0; i < m_locales.Size(); ++i)
                if (SameLocale(m_locales[i].Id.AsView(), locale))
                    return i;
            return kNone;
        }
        usize EnsureLocale(StringView locale)
        {
            usize at = FindLocale(locale);
            if (at == kNone && AddLocale(locale))
                at = m_locales.Size() - 1;
            return at;
        }
        [[nodiscard]] usize FindEntry(StringView key) const noexcept
        {
            if (const u32* at = m_index.Find(HashText(key)); at != nullptr && m_entries[*at].Key.AsView() == key)
                return *at;
            for (usize i = 0; i < m_entries.Size(); ++i) // a hash shared by two keys
                if (m_entries[i].Key.AsView() == key)
                    return i;
            return kNone;
        }
        Entry& EnsureEntry(StringView key, bool plural)
        {
            if (const usize at = FindEntry(key); at != kNone)
                return m_entries[at];
            Entry e;
            e.Key = String(key);
            e.Plural = plural;
            e.Cells.Resize(m_locales.Size());
            m_entries.PushBack(Move(e));
            m_index.InsertOrAssign(HashText(key), static_cast<u32>(m_entries.Size() - 1));
            return m_entries[m_entries.Size() - 1];
        }
        [[nodiscard]] const Cell* FindCell(StringView key, StringView locale) const noexcept
        {
            const usize at = FindEntry(key);
            const usize column = FindLocale(locale);
            return at != kNone && column != kNone ? &m_entries[at].Cells[column] : nullptr;
        }
        void Reindex()
        {
            m_index = HashMap<u64, u32>{};
            for (usize i = 0; i < m_entries.Size(); ++i)
                m_index.InsertOrAssign(HashText(m_entries[i].Key.AsView()), static_cast<u32>(i));
        }

        Array<LocaleInfo> m_locales;
        Array<Entry> m_entries;
        HashMap<u64, u32> m_index; // a key's hash to its entry
    };

    /// Registers StringTable with the type registry and the serializable factory (idempotent).
    void RegisterStringTableType();
}
