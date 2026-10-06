// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Settings - :save_values partition.
//
// A settings section whose fields are not known ahead: a game's saved values (a best time, a high
// score, an unlocked level, its own options), each a key and a typed value. A save file is then an
// ordinary settings file holding one SaveValues section, with the store's envelope, versioning and
// unknown-section passthrough. Kept sorted by key, so a saved file reads (and diffs) the same way
// every time. See Documentation/Specs/save-data.md.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.settings:save_values;

import foundation.core;

using namespace foundation::core;

export namespace foundation::settings
{
    enum class SaveValueKind : u8
    {
        Bool,
        Int,
        Float,
        Text,
        Floats, // a list of numbers (a recorded run, a curve)
    };

    class SaveValues final : public ISerializable
    {
        RTTI_OBJECT(SaveValues, ISerializable)
    public:
        struct Entry
        {
            String key;
            SaveValueKind kind = SaveValueKind::Int;
            bool boolValue = false;
            i32 intValue = 0;
            f32 floatValue = 0.0f;
            String textValue;
            Array<f32> floatsValue;
        };

        [[nodiscard]] usize Count() const noexcept { return m_entries.Size(); }
        [[nodiscard]] const Entry& At(usize index) const noexcept { return m_entries[index]; }
        [[nodiscard]] bool Has(StringView key) const noexcept { return Find(key) != nullptr; }

        // Each setter answers whether the stored value changed (a new key, another kind or another
        // value), which is what tells the owner the file needs writing.
        bool SetBool(StringView key, bool value)
        {
            Entry& entry = Slot(key);
            const bool changed = entry.kind != SaveValueKind::Bool || entry.boolValue != value || m_inserted;
            entry = Entry{entry.key, SaveValueKind::Bool, value, 0, 0.0f, String{}, Array<f32>{}};
            return changed;
        }
        bool SetInt(StringView key, i32 value)
        {
            Entry& entry = Slot(key);
            const bool changed = entry.kind != SaveValueKind::Int || entry.intValue != value || m_inserted;
            entry = Entry{entry.key, SaveValueKind::Int, false, value, 0.0f, String{}, Array<f32>{}};
            return changed;
        }
        bool SetFloat(StringView key, f32 value)
        {
            Entry& entry = Slot(key);
            const bool changed = entry.kind != SaveValueKind::Float || entry.floatValue != value || m_inserted;
            entry = Entry{entry.key, SaveValueKind::Float, false, 0, value, String{}, Array<f32>{}};
            return changed;
        }
        bool SetText(StringView key, StringView value)
        {
            Entry& entry = Slot(key);
            const bool changed =
                entry.kind != SaveValueKind::Text || entry.textValue.AsView() != value || m_inserted;
            entry = Entry{entry.key, SaveValueKind::Text, false, 0, 0.0f, String(value), Array<f32>{}};
            return changed;
        }

        bool SetFloats(StringView key, Span<const f32> values)
        {
            Entry& entry = Slot(key);
            bool changed = entry.kind != SaveValueKind::Floats || entry.floatsValue.Size() != values.Size() || m_inserted;
            for (usize i = 0; !changed && i < values.Size(); ++i)
            {
                changed = entry.floatsValue[i] != values[i];
            }
            Array<f32> copy;
            copy.Reserve(values.Size());
            for (f32 v : values)
            {
                copy.PushBack(v);
            }
            entry = Entry{entry.key, SaveValueKind::Floats, false, 0, 0.0f, String{}, Move(copy)};
            return changed;
        }

        // A value read as another kind than it was written answers the fallback, except that an
        // int reads as a float: a whole number is still a number.
        [[nodiscard]] bool GetBool(StringView key, bool fallback) const noexcept
        {
            const Entry* entry = Find(key);
            return (entry != nullptr && entry->kind == SaveValueKind::Bool) ? entry->boolValue : fallback;
        }
        [[nodiscard]] i32 GetInt(StringView key, i32 fallback) const noexcept
        {
            const Entry* entry = Find(key);
            return (entry != nullptr && entry->kind == SaveValueKind::Int) ? entry->intValue : fallback;
        }
        [[nodiscard]] f32 GetFloat(StringView key, f32 fallback) const noexcept
        {
            const Entry* entry = Find(key);
            if (entry == nullptr)
            {
                return fallback;
            }
            if (entry->kind == SaveValueKind::Float)
            {
                return entry->floatValue;
            }
            return entry->kind == SaveValueKind::Int ? static_cast<f32>(entry->intValue) : fallback;
        }
        [[nodiscard]] String GetText(StringView key, StringView fallback) const
        {
            const Entry* entry = Find(key);
            return (entry != nullptr && entry->kind == SaveValueKind::Text) ? entry->textValue
                                                                              : String(fallback);
        }

        // The list stored at `key`, or an empty one when there is none or the value is another kind.
        [[nodiscard]] Array<f32> GetFloats(StringView key) const
        {
            const Entry* entry = Find(key);
            return (entry != nullptr && entry->kind == SaveValueKind::Floats) ? entry->floatsValue : Array<f32>{};
        }

        // Answers whether there was a value to remove.
        bool Remove(StringView key)
        {
            const usize at = LowerBound(key);
            if (at < m_entries.Size() && m_entries[at].key.AsView() == key)
            {
                m_entries.RemoveAt(at);
                return true;
            }
            return false;
        }
        // Answers whether there was anything to clear.
        bool Clear()
        {
            const bool had = !m_entries.IsEmpty();
            m_entries.Clear();
            return had;
        }

        void Serialize(ISerializer& ar) override;

    private:
        [[nodiscard]] usize LowerBound(StringView key) const noexcept
        {
            usize low = 0;
            usize high = m_entries.Size();
            while (low < high)
            {
                const usize mid = low + (high - low) / 2;
                if (m_entries[mid].key.AsView().Compare(key) < 0)
                {
                    low = mid + 1;
                }
                else
                {
                    high = mid;
                }
            }
            return low;
        }
        [[nodiscard]] const Entry* Find(StringView key) const noexcept
        {
            const usize at = LowerBound(key);
            return (at < m_entries.Size() && m_entries[at].key.AsView() == key) ? &m_entries[at]
                                                                                  : nullptr;
        }
        // The entry for `key`, inserted in order if new (m_inserted says which).
        Entry& Slot(StringView key)
        {
            const usize at = LowerBound(key);
            m_inserted = !(at < m_entries.Size() && m_entries[at].key.AsView() == key);
            if (m_inserted)
            {
                Entry entry;
                entry.key = String(key);
                m_entries.Insert(at, Move(entry));
            }
            return m_entries[at];
        }

        Array<Entry> m_entries;
        bool m_inserted = false;
    };

    // Load needs the section resolvable by name and constructible by id; idempotent.
    void RegisterSaveValuesType();
}
