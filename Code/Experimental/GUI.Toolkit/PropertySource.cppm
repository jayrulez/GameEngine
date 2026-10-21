// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :property_source partition
//
// What a property grid reads (T1 of Specs/gui-lab-evolution-3.md): a source of properties over one
// object or several. A property is one value (a bool, an integer, a float, a string, an enum or
// flags) at a path; a group (a vector's parts in T2, a nested object) is a property whose children
// are the paths under it (`position.x`), so values that differ between the objects differ per
// part. The source says what is there and what each object holds; the grid edits through it as
// gestures: begun, previewed live on every object, then committed (the originals put back and the
// edit applied once, one undo entry) or cancelled (the originals put back). The undo is the
// source's (an application's, as ezEngine's and Zero's), never the grid's.
//
// PropertyBag is a source of its own values in memory: what tests and a page without objects of
// its own edit, its commits told to whoever keeps the undo.

module;
#include "Core/Prelude.h"

export module experimental.gui.toolkit:property_source;

import foundation.core;
import experimental.gui;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    enum class PropertyKind : u8
    {
        Group,   ///< no value of its own: its children are the paths under it
        Bool,
        Integer,
        Float,
        String,
        Enum,    ///< an integer, one of its choices
        Flags,   ///< an integer, any of its choices' bits
    };

    /// An enum's value or a flag's bit, and what it is called.
    struct PropertyChoice
    {
        core::String Name;
        i64 Value = 0;
    };

    /// What a source says of one property. Read afresh each time the grid asks, so whether it shows
    /// or may be edited can follow other values (T3's `visibleWhen`).
    struct PropertyInfo
    {
        core::String Name;        ///< its segment of the path
        core::String DisplayName; ///< what its row says; empty: its name
        core::String Category;    ///< a top-level property's group in the grid; empty: none
        core::String Description; ///< its row's tooltip
        PropertyKind Kind = PropertyKind::String;
        core::String Hint;        ///< an editor asked for by name ("slider"), before its kind's
        bool HasRange = false;    ///< a number kept between Min and Max; else unbounded
        f64 Min = 0.0;
        f64 Max = 0.0;
        f64 Step = 0.0;           ///< a number's step; 0: the editor's own
        core::Array<PropertyChoice> Choices; ///< an enum's values, flags' bits
        bool ReadOnly = false;
        bool Visible = true;

        [[nodiscard]] core::StringView ShownName() const noexcept { return DisplayName.Size() != 0 ? DisplayName.AsView() : Name.AsView(); }
    };

    /// A value written by an edit. For flags only `Mask`'s bits change, each object keeping its
    /// others, so a flag set on several objects leaves the flags they differ in as they were.
    struct PropertyWrite
    {
        Variant Value;
        u64 Mask = ~u64{0};
    };

    /// What a source tells its grid.
    class IPropertySourceClient
    {
    public:
        virtual ~IPropertySourceClient() = default;
        /// Which properties there are, or what they say of themselves, changed: read them again.
        virtual void OnPropertiesChanged() = 0;
        /// Values changed (an undo, another editor, the objects themselves): show them again.
        virtual void OnValuesChanged() = 0;
    };

    class IPropertySource
    {
    public:
        virtual ~IPropertySource() = default;

        /// How many objects it stands for; their values differing is a row's mixed state.
        [[nodiscard]] virtual usize ObjectCount() const = 0;
        /// The properties under `parent` (empty: the top level), in their order.
        virtual void GetProperties(core::StringView parent, core::Array<PropertyInfo>& out) const = 0;
        /// The value at `path` of object `object`.
        [[nodiscard]] virtual Variant GetValue(core::StringView path, usize object) const = 0;

        /// An edit of `path` begins: what each object holds now is what a cancel puts back.
        virtual void BeginEdit(core::StringView path) = 0;
        /// The edit as it goes (a scrub, a picker open): written to every object, live.
        virtual void PreviewEdit(core::StringView path, const PropertyWrite& write) = 0;
        /// The edit ends with `write`: the originals put back, then `write` applied as one change.
        virtual void CommitEdit(core::StringView path, const PropertyWrite& write) = 0;
        /// The edit is given up: the originals put back.
        virtual void CancelEdit(core::StringView path) = 0;

        void SetClient(IPropertySourceClient* client) noexcept { m_client = client; }
        [[nodiscard]] IPropertySourceClient* GetClient() const noexcept { return m_client; }

    protected:
        void NotifyPropertiesChanged()
        {
            if (m_client != nullptr)
                m_client->OnPropertiesChanged();
        }
        void NotifyValuesChanged()
        {
            if (m_client != nullptr)
                m_client->OnValuesChanged();
        }

    private:
        IPropertySourceClient* m_client = nullptr;
    };

    /// `path` joined under `parent` ("position" and "x": "position.x").
    [[nodiscard]] inline core::String JoinPropertyPath(core::StringView parent, core::StringView name)
    {
        core::String path(parent);
        if (path.Size() != 0)
            path.Append(core::StringView(u8"."));
        path.Append(name);
        return path;
    }
    /// The path `path` is under ("" for a top-level property).
    [[nodiscard]] inline core::StringView ParentPropertyPath(core::StringView path)
    {
        for (usize i = path.Size(); i > 0; --i)
            if (path[i - 1] == u8'.')
                return path.SubStr(0, i - 1);
        return core::StringView{};
    }

    /// `write` applied to `old`: flags keep the bits outside its mask.
    [[nodiscard]] inline Variant ApplyPropertyWrite(const Variant& old, const PropertyWrite& write)
    {
        if (write.Mask == ~u64{0})
            return write.Value;
        const u64 bits = (static_cast<u64>(old.AsInt()) & ~write.Mask) | (static_cast<u64>(write.Value.AsInt()) & write.Mask);
        return Variant(static_cast<i64>(bits));
    }

    /// A property's value over every object: the first object's, whether any differs, and for
    /// flags which bits differ.
    struct PropertyValue
    {
        Variant Value;
        bool Mixed = false;
        u64 MixedBits = 0;
    };

    [[nodiscard]] inline PropertyValue ReadPropertyValue(const IPropertySource& source, core::StringView path, PropertyKind kind)
    {
        PropertyValue out;
        const usize count = source.ObjectCount();
        if (count == 0)
            return out;
        out.Value = source.GetValue(path, 0);
        for (usize i = 1; i < count; ++i)
        {
            const Variant other = source.GetValue(path, i);
            if (kind == PropertyKind::Flags)
                out.MixedBits |= static_cast<u64>(out.Value.AsInt()) ^ static_cast<u64>(other.AsInt());
            else if (out.Value.GetType() != other.GetType() || out.Value.Compare(other) != 0)
                out.Mixed = true;
        }
        if (out.MixedBits != 0)
            out.Mixed = true;
        return out;
    }

    /// A source of its own values in memory: properties declared by path, each object's values
    /// kept here. A commit is told (path, each object's value before, each after) to whoever keeps
    /// the undo, which puts values back with SetValue.
    class PropertyBag : public IPropertySource
    {
    public:
        struct Commit
        {
            core::String Path;
            core::Array<Variant> Before;
            core::Array<Variant> After;
        };

        explicit PropertyBag(core::IAllocator& allocator, usize objects = 1)
            : m_allocator(&allocator), m_properties(allocator), m_originals(allocator), m_objects(objects)
        {
        }

        /// Declares the property `path` (its name the last segment, under its parent's path) with
        /// `value` for every object; returns its info to fill in.
        PropertyInfo& Add(core::StringView path, PropertyKind kind, const Variant& value = {})
        {
            Entry entry;
            entry.Path = core::String(path);
            const usize parent = ParentPropertyPath(path).Size();
            entry.Info.Name = core::String(parent == 0 ? path : path.SubStr(parent + 1, path.Size() - parent - 1));
            entry.Info.Kind = kind;
            entry.Values = core::Array<Variant>(*m_allocator);
            for (usize i = 0; i < m_objects; ++i)
                entry.Values.PushBack(value);
            m_properties.PushBack(core::Move(entry));
            NotifyPropertiesChanged();
            return m_properties.Back().Info;
        }
        /// The info of `path`, to change; NotifyPropertiesChanged after.
        [[nodiscard]] PropertyInfo* FindInfo(core::StringView path)
        {
            Entry* entry = Find(path);
            return entry != nullptr ? &entry->Info : nullptr;
        }
        /// Tells the grid what FindInfo's changes were.
        void PropertiesChanged() { NotifyPropertiesChanged(); }

        /// Sets object `object`'s value at `path` from code (an undo, a page's own change).
        void SetValue(core::StringView path, usize object, const Variant& value)
        {
            Entry* entry = Find(path);
            if (entry == nullptr || object >= entry->Values.Size())
                return;
            entry->Values[object] = value;
            NotifyValuesChanged();
        }

        /// Told each commit, to keep it for undo.
        void SetOnCommit(core::Function<void(const Commit&)> onCommit) { m_onCommit = core::Move(onCommit); }
        /// The edits begun and not yet ended.
        [[nodiscard]] usize OpenEdits() const noexcept { return m_originals.Size(); }

        [[nodiscard]] usize ObjectCount() const override { return m_objects; }
        void GetProperties(core::StringView parent, core::Array<PropertyInfo>& out) const override
        {
            for (const Entry& entry : m_properties)
                if (ParentPropertyPath(entry.Path.AsView()) == parent)
                    out.PushBack(entry.Info);
        }
        [[nodiscard]] Variant GetValue(core::StringView path, usize object) const override
        {
            const Entry* entry = Find(path);
            return entry != nullptr && object < entry->Values.Size() ? entry->Values[object] : Variant{};
        }

        void BeginEdit(core::StringView path) override
        {
            Entry* entry = Find(path);
            if (entry == nullptr || FindOriginal(path) != nullptr)
                return;
            Original original;
            original.Path = core::String(path);
            original.Values = entry->Values;
            m_originals.PushBack(core::Move(original));
        }
        void PreviewEdit(core::StringView path, const PropertyWrite& write) override
        {
            Entry* entry = Find(path);
            if (entry == nullptr || entry->Info.ReadOnly)
                return;
            Write(*entry, write);
            NotifyValuesChanged();
        }
        void CommitEdit(core::StringView path, const PropertyWrite& write) override
        {
            Entry* entry = Find(path);
            if (entry == nullptr || entry->Info.ReadOnly)
            {
                EndEdit(path);
                return;
            }
            Commit commit;
            commit.Path = core::String(path);
            const Original* original = FindOriginal(path);
            commit.Before = original != nullptr ? original->Values : entry->Values;
            entry->Values = commit.Before; // the originals back, then the one change
            Write(*entry, write);
            commit.After = entry->Values;
            EndEdit(path);
            NotifyValuesChanged();
            if (m_onCommit)
                m_onCommit(commit);
        }
        void CancelEdit(core::StringView path) override
        {
            Entry* entry = Find(path);
            const Original* original = FindOriginal(path);
            if (entry != nullptr && original != nullptr)
                entry->Values = original->Values;
            EndEdit(path);
            NotifyValuesChanged();
        }

    private:
        struct Entry
        {
            core::String Path;
            PropertyInfo Info;
            core::Array<Variant> Values;
        };
        struct Original
        {
            core::String Path;
            core::Array<Variant> Values;
        };

        const Entry* Find(core::StringView path) const
        {
            for (const Entry& entry : m_properties)
                if (entry.Path.AsView() == path)
                    return &entry;
            return nullptr;
        }
        Entry* Find(core::StringView path) { return const_cast<Entry*>(static_cast<const PropertyBag*>(this)->Find(path)); }
        const Original* FindOriginal(core::StringView path) const
        {
            for (const Original& original : m_originals)
                if (original.Path.AsView() == path)
                    return &original;
            return nullptr;
        }
        void EndEdit(core::StringView path)
        {
            for (usize i = 0; i < m_originals.Size(); ++i)
                if (m_originals[i].Path.AsView() == path)
                {
                    m_originals.RemoveAt(i);
                    return;
                }
        }
        static void Write(Entry& entry, const PropertyWrite& write)
        {
            for (Variant& value : entry.Values)
                value = ApplyPropertyWrite(value, write);
        }

        core::IAllocator* m_allocator;
        core::Array<Entry> m_properties;
        core::Array<Original> m_originals;
        usize m_objects;
        core::Function<void(const Commit&)> m_onCommit;
    };
}
