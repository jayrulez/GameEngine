// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.i18n - StringTable's type definition and its payload.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.i18n;

import foundation.core;

using namespace foundation::core;

namespace foundation::i18n
{
    RTTI_DEFINE_OBJECT_VERSIONED(StringTable, "rtti::foundation::i18n", 1)

    namespace
    {
        // Directions are written by name, so a table reads in a text editor.
        constexpr StringView kDirectionNames[] = {u8"", u8"ltr", u8"rtl"};

        TextDirection DirectionNamed(StringView name) noexcept
        {
            for (usize i = 0; i < ArrayCount(kDirectionNames); ++i)
                if (kDirectionNames[i] == name)
                    return static_cast<TextDirection>(i);
            return TextDirection::Unset;
        }
    }

    // The payload: the locales (each its id, its plural rule's text when it has one, its
    // direction), then the keys (each plain or plural, a cell a locale: a text, or the plural
    // forms by category name). A rule read back that does not parse fails the read.
    void StringTable::Serialize(ISerializer& ar)
    {
        BeginVersionedPayload(ar, StaticType());
        const bool reading = ar.Mode() == SerializeMode::Read;

        u32 localeCount = static_cast<u32>(m_locales.Size());
        ar.Key("locales");
        ar.BeginArray(localeCount);
        if (reading)
            m_locales.Clear();
        for (u32 i = 0; i < localeCount; ++i)
        {
            LocaleInfo locale;
            String rule;
            String direction;
            if (!reading)
            {
                locale.Id = m_locales[i].Id;
                locale.HasRule = m_locales[i].HasRule;
                rule = String(m_locales[i].Rule.Source());
                direction = String(kDirectionNames[static_cast<usize>(m_locales[i].Direction)]);
            }
            ar.BeginObject();
            foundation::core::Serialize(ar, "id", locale.Id);
            foundation::core::Serialize(ar, "hasRule", locale.HasRule);
            foundation::core::Serialize(ar, "rule", rule);
            foundation::core::Serialize(ar, "direction", direction);
            ar.EndObject();
            if (reading)
            {
                locale.Direction = DirectionNamed(direction.AsView());
                if (locale.HasRule && !locale.Rule.Parse(rule.AsView()))
                    ar.FailPayload(ErrorCode::InvalidArgument);
                m_locales.PushBack(Move(locale));
            }
        }
        ar.EndArray();

        u32 entryCount = static_cast<u32>(m_entries.Size());
        ar.Key("entries");
        ar.BeginArray(entryCount);
        if (reading)
            m_entries.Clear();
        for (u32 i = 0; i < entryCount; ++i)
        {
            Entry entry;
            if (!reading)
                entry = m_entries[i];
            ar.BeginObject();
            foundation::core::Serialize(ar, "key", entry.Key);
            foundation::core::Serialize(ar, "plural", entry.Plural);
            u32 cellCount = static_cast<u32>(entry.Cells.Size());
            ar.Key("cells");
            ar.BeginArray(cellCount);
            if (reading)
                entry.Cells.Resize(cellCount);
            for (u32 c = 0; c < cellCount; ++c)
            {
                Cell& cell = entry.Cells[c];
                ar.BeginObject();
                foundation::core::Serialize(ar, "present", cell.Present);
                foundation::core::Serialize(ar, "text", cell.Text);
                foundation::core::Serialize(ar, "forms", cell.FormsPresent);
                for (u32 f = 0; f < kPluralCategoryCount; ++f)
                {
                    // Each form under its category's name ("one", "other"), a keyed table readable.
                    const String name(PluralCategoryName(static_cast<PluralCategory>(f)));
                    ar.Key(reinterpret_cast<const char*>(name.CStr()));
                    ar.Text(cell.Forms[f]);
                }
                ar.EndObject();
            }
            ar.EndArray();
            ar.EndObject();
            if (reading)
            {
                if (entry.Cells.Size() != m_locales.Size())
                    ar.FailPayload(ErrorCode::InvalidArgument); // a cell a locale, or the table is not one
                m_entries.PushBack(Move(entry));
            }
        }
        ar.EndArray();
        if (reading)
            Reindex();
        EndVersionedPayload(ar);
    }

    void RegisterStringTableType()
    {
        static const bool once = []()
        {
            GlobalTypeRegistry().Register(StringTable::StaticType());
            RegisterSerializable<StringTable>();
            return true;
        }();
        (void)once;
    }
}
