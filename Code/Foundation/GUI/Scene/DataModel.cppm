// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :data_model partition
//
// DataModel: a GUI's named values, which markup and scripts bind to. Each name is an observable
// value, so a binding that reads `score` follows it without subscribing. A binding that reads a
// name before it is set still follows it and picks up the first value when it arrives. Values are
// Variants, written by the game, a script or a widget.
//
// The conversions that bindings need are here too. A value may feed a property of another type (a
// script's number into a slider's f32, a count into a label's text), and values compare equal
// across numeric types, so writing back a value that was just read is not a change.

module;
#include "Core/Prelude.h"

export module foundation.gui:data_model;

import foundation.core;
import :reactive;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    namespace detail
    {
        // Reads a numeric Variant's value as f64 (a bool counts as 0 or 1). Returns false when it
        // holds no number.
        [[nodiscard]] inline bool NumberOf(const Variant& v, f64& out)
        {
            if (const f64* d = v.TryGet<f64>()) { out = *d; return true; }
            if (const f32* f = v.TryGet<f32>()) { out = *f; return true; }
            if (const i32* i = v.TryGet<i32>()) { out = *i; return true; }
            if (const i64* l = v.TryGet<i64>()) { out = static_cast<f64>(*l); return true; }
            if (const u32* u = v.TryGet<u32>()) { out = *u; return true; }
            if (const u64* q = v.TryGet<u64>()) { out = static_cast<f64>(*q); return true; }
            if (const bool* b = v.TryGet<bool>()) { out = *b ? 1.0 : 0.0; return true; }
            return false;
        }

        [[nodiscard]] inline core::StringView TextOf(const Variant& v)
        {
            if (const core::String* s = v.TryGet<core::String>())
                return s->AsView();
            if (const core::StringView* view = v.TryGet<core::StringView>())
                return *view;
            return {};
        }
        [[nodiscard]] inline bool IsText(const Variant& v)
        {
            return v.TryGet<core::String>() != nullptr || v.TryGet<core::StringView>() != nullptr;
        }
    }

    /// Formats `value` as text: a string as it is, a whole number without a fraction, another
    /// number in its shortest form, and a boolean as true or false. Other types give empty text.
    [[nodiscard]] inline core::String FormatVariant(const Variant& value)
    {
        if (detail::IsText(value))
            return core::String(detail::TextOf(value));
        if (const bool* b = value.TryGet<bool>())
            return core::String(*b ? core::StringView(u8"true") : core::StringView(u8"false"));
        f64 n = 0.0;
        if (!detail::NumberOf(value, n))
            return {};
        if (const f32* f = value.TryGet<f32>(); f != nullptr && *f != static_cast<f32>(static_cast<i64>(*f)))
            return Format(u8"{}", *f); // the f32's shortest form (0.4), not that of its f64 widening
        if (Abs(n) < 1e15 && n == static_cast<f64>(static_cast<i64>(n)))
            return Format(u8"{}", static_cast<i64>(n));
        return Format(u8"{}", n); // the shortest form that reads back as the same number
    }

    /// Converts `value` to a Variant of `target`'s type. Numbers convert among numeric types and
    /// from text, anything formats to text, and numbers or "true"/"false" convert to bool. Returns
    /// an empty Variant when no conversion applies.
    [[nodiscard]] inline Variant ConvertVariant(const Variant& value, const TypeInfo& target)
    {
        if (value.IsEmpty())
            return {};
        if (value.Type() == &target)
            return value;
        if (&target == &TypeOf<core::String>())
            return Variant::From<core::String>(FormatVariant(value));
        f64 n = 0.0;
        bool numeric = detail::NumberOf(value, n);
        if (!numeric && detail::IsText(value))
        {
            const core::StringView text = Trim(detail::TextOf(value));
            if (&target == &TypeOf<bool>() && (text == core::StringView(u8"true") || text == core::StringView(u8"false")))
                return Variant::From<bool>(text == core::StringView(u8"true"));
            if (Optional<f64> parsed = ParseFloat(text); parsed.HasValue())
            {
                n = parsed.Value();
                numeric = true;
            }
        }
        if (!numeric)
            return {};
        const i64 whole = static_cast<i64>(n < 0.0 ? n - 0.5 : n + 0.5); // rounded to the nearest
        if (&target == &TypeOf<f32>()) return Variant::From<f32>(static_cast<f32>(n));
        if (&target == &TypeOf<f64>()) return Variant::From<f64>(n);
        if (&target == &TypeOf<i32>()) return Variant::From<i32>(static_cast<i32>(whole));
        if (&target == &TypeOf<i64>()) return Variant::From<i64>(whole);
        if (&target == &TypeOf<u32>()) return Variant::From<u32>(static_cast<u32>(whole < 0 ? 0 : whole));
        if (&target == &TypeOf<u64>()) return Variant::From<u64>(static_cast<u64>(whole < 0 ? 0 : whole));
        if (&target == &TypeOf<bool>()) return Variant::From<bool>(n != 0.0);
        return {};
    }

    /// Whether two values are the same: numbers compare across numeric types, text compares by
    /// bytes, and two empty values are equal. Other types are equal only when both are empty,
    /// since they cannot be compared generically.
    [[nodiscard]] inline bool VariantsEqual(const Variant& a, const Variant& b)
    {
        if (a.IsEmpty() || b.IsEmpty())
            return a.IsEmpty() && b.IsEmpty();
        f64 x = 0.0, y = 0.0;
        if (detail::NumberOf(a, x) && detail::NumberOf(b, y))
            return x == y;
        if (detail::IsText(a) && detail::IsText(b))
            return detail::TextOf(a) == detail::TextOf(b);
        return false;
    }

    /// Shared by its GUIContext and every binding to it (MakeRef), so the model outlives every
    /// binding.
    class DataModel : public RefCounted
    {
    public:
        DataModel() noexcept = default;
        DataModel(const DataModel&) = delete;
        DataModel& operator=(const DataModel&) = delete;

        /// Returns the value named `name` (empty when unset). A binding records the read whether or
        /// not the name is set.
        [[nodiscard]] Variant Get(core::StringView name)
        {
            Entry& entry = EntryFor(name);
            entry.Changed->Track();
            return entry.Value;
        }
        /// Sets `name`, and every binding that reads it follows. Returns false when it already held
        /// that value.
        bool Set(core::StringView name, Variant value)
        {
            Entry& entry = EntryFor(name);
            if (entry.HasValue && VariantsEqual(entry.Value, value))
                return false;
            entry.Value = Move(value);
            entry.HasValue = true;
            entry.Changed->Notify();
            return true;
        }
        /// Returns the value named `name` without a binding recording the read (a tween reads its
        /// start value this way).
        [[nodiscard]] Variant Peek(core::StringView name) const
        {
            for (const Entry& entry : m_entries)
                if (entry.Name.AsView() == name)
                    return entry.Value;
            return {};
        }
        /// Records `owner` as the tween now animating `name`. A starting tween claims the name, so
        /// the previous tween sees it was replaced and stops where it is. The owner is 0 when no
        /// tween has claimed it.
        void ClaimTween(core::StringView name, u64 owner) { EntryFor(name).TweenOwner = owner; }
        [[nodiscard]] u64 TweenOwnerOf(core::StringView name) const
        {
            for (const Entry& entry : m_entries)
                if (entry.Name.AsView() == name)
                    return entry.TweenOwner;
            return 0;
        }
        [[nodiscard]] bool Has(core::StringView name) const
        {
            for (const Entry& entry : m_entries)
                if (entry.Name.AsView() == name)
                    return entry.HasValue;
            return false;
        }

    private:
        struct Entry
        {
            core::String Name;
            Variant Value;
            bool HasValue = false;
            u64 TweenOwner = 0; // the tween animating it
            UniquePtr<Observable> Changed; // boxed because Observable is not movable and the entry array moves
        };
        Array<Entry> m_entries;

        Entry& EntryFor(core::StringView name)
        {
            for (Entry& entry : m_entries)
                if (entry.Name.AsView() == name)
                    return entry;
            Entry entry;
            entry.Name = core::String(name);
            entry.Changed = MakeUnique<Observable>(MemoryAllocator());
            m_entries.PushBack(Move(entry));
            return m_entries[m_entries.Size() - 1];
        }
    };
}
