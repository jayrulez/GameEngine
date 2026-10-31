// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :property_source partition
//
// What a property grid reads: a source of properties over one object or several. A property is
// one value (a bool, an integer, a float, a string, an enum or flags) at a path. A group (a
// vector's parts, a nested object) is a property whose children are the paths under it
// (`position.x`), so values that differ between the objects differ per part. The source says what
// properties there are and what each object holds. The grid edits through it in gestures: a gesture
// begins, previews live on every object, and then either commits (the originals are put back and
// the edit applied once, as one undo entry) or cancels (the originals are put back). The undo
// belongs to the source, that is, to the application, and never to the grid.
//
// A source may give each object's default for a path. The grid marks values that differ from it,
// and a revert restores each object's own default as one undo entry.
//
// Copying and pasting go through the source as edits do. The source writes a path and what is
// under it as text, and reads it back as one undo entry. The grid provides the menu and the keys,
// and knows no format.
//
// A list's elements are its children by index. A change to the list (an insert, a removal or a
// move) is one call and one undo entry, made in each object.
//
// PropertyBag is a source that holds its own values in memory. Tests use it, as does a page with no
// objects of its own. It reports its commits to whoever keeps the undo.

module;
#include "Core/Prelude.h"

export module foundation.gui.toolkit:property_source;

import foundation.core;
import foundation.gui;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    enum class PropertyKind : u8
    {
        Group,   ///< has no value of its own; its children are the paths under it
        Bool,
        Integer,
        Float,
        String,
        Enum,    ///< an integer holding one of its choices
        Flags,   ///< an integer holding any of its choices' bits
        List,    ///< its elements are its children by index (`tags.0`); its value is their count
        Color,   ///< a colour, as sRGB values
        Curve,   ///< Core's curve keys as one value; MaxCount limits the keys, and a range bounds the values
        Gradient, ///< a ColorGradient as one value; MaxCount limits the stops of each kind
        Reference, ///< the Guid, as text, of something kept elsewhere (an asset, an entity); ReferenceType is what it points at
    };

    /// What a list allows (PropertyInfo::ListOps).
    enum PropertyListOps : u8
    {
        PropertyListAdd = 1u << 0,
        PropertyListRemove = 1u << 1,
        PropertyListMove = 1u << 2,
    };

    /// An enum's value or a flag's bit, and its name.
    struct PropertyChoice
    {
        core::String Name;
        i64 Value = 0;
    };

    /// What a source reports about one property. It is read afresh each time the grid asks, so
    /// whether it shows or may be edited can depend on other values (a reflected source's
    /// `visibleWhen`).
    struct PropertyInfo
    {
        core::String Name;        ///< its segment of the path
        core::String DisplayName; ///< what its row says; empty shows its name
        core::String Category;    ///< a top-level property's category in the grid; empty for none
        core::String Description; ///< its row's tooltip
        PropertyKind Kind = PropertyKind::String;
        core::String Hint;        ///< an editor requested by name ("slider"), chosen before its kind's editor
        bool HasRange = false;    ///< a number is kept between Min and Max; otherwise it is unbounded
        f64 Min = 0.0;
        f64 Max = 0.0;
        f64 Step = 0.0;           ///< a number's step; 0 uses the editor's default
        core::Array<PropertyChoice> Choices; ///< an enum's values, or the flags' bits
        bool ReadOnly = false;
        bool Visible = true;
        core::String Summary; ///< for a group, a one-line summary shown on its row while it is closed
        bool ShowsAlpha = true;   ///< whether a colour's alpha is edited (not for an HDR colour, nor one with `noAlpha`)
        u8 ListOps = 0;           ///< for a list, its PropertyListOps
        u32 MaxCount = 0;         ///< the most elements of a list (a BoundedArray's), keys of a curve or stops of a gradient; 0 for no limit
        core::Array<core::String> ElementTypes; ///< the types an add to a list chooses from; empty for its single type
        const core::TypeInfo* ReferenceType = nullptr; ///< for a reference, the type it points at (the application finds what may fill it)

        [[nodiscard]] core::StringView ShownName() const noexcept { return DisplayName.Size() != 0 ? DisplayName.AsView() : Name.AsView(); }
    };

    /// A value written by an edit. For flags only the bits in `Mask` change and each object keeps
    /// its other bits, so setting a flag on several objects leaves the flags they differ in as they
    /// were.
    struct PropertyWrite
    {
        Variant Value;
        u64 Mask = ~u64{0};
    };

    /// A change to a list: insert an element at Index (of ElementType, for a list of several
    /// types), remove the one at Index, or move it to To (its index after the move).
    struct ListEdit
    {
        enum class Op : u8
        {
            Insert,
            Remove,
            Move,
        };
        Op Kind = Op::Insert;
        usize Index = 0;
        usize To = 0;
        core::String ElementType;
    };

    /// The notifications a source sends its grid.
    class IPropertySourceClient
    {
    public:
        virtual ~IPropertySourceClient() = default;
        /// Called when the set of properties or their info changed; the grid reads them again.
        virtual void OnPropertiesChanged() = 0;
        /// Called when values changed (by an undo, another editor or the objects themselves); the
        /// grid shows them again.
        virtual void OnValuesChanged() = 0;
    };

    class IPropertySource
    {
    public:
        virtual ~IPropertySource() = default;

        /// Returns how many objects it stands for. When their values differ, a row shows as mixed.
        [[nodiscard]] virtual usize ObjectCount() const = 0;
        /// Fills `out` with the properties under `parent` (empty for the top level), in order.
        virtual void GetProperties(core::StringView parent, core::Array<PropertyInfo>& out) const = 0;
        /// Returns the value at `path` of object `object`.
        [[nodiscard]] virtual Variant GetValue(core::StringView path, usize object) const = 0;

        /// Begins an edit of `path`. What each object holds now is what a cancel restores.
        virtual void BeginEdit(core::StringView path) = 0;
        /// Writes the edit live to every object while it goes on (a scrub, an open picker).
        virtual void PreviewEdit(core::StringView path, const PropertyWrite& write) = 0;
        /// Ends the edit with `write`: puts the originals back, then applies `write` as one change.
        virtual void CommitEdit(core::StringView path, const PropertyWrite& write) = 0;
        /// Cancels the edit and puts the originals back.
        virtual void CancelEdit(core::StringView path) = 0;
        /// Changes the list `path` in each object as one undo entry. Returns false when nothing
        /// changed.
        virtual bool EditList(core::StringView path, const ListEdit& edit) = 0;

        /// Returns what object `object` would hold at `path` by default, or empty when it has none.
        /// The source decides what the default is (the type's, a prefab's).
        [[nodiscard]] virtual Variant GetDefault(core::StringView path, usize object) const = 0;
        /// Restores each object's own default at `path` and under it, as one undo entry. Returns
        /// false when nothing changed.
        virtual bool Revert(core::StringView path) = 0;
        /// Returns the menu text for reverting `path` ("Revert to default", "Revert to prefab").
        [[nodiscard]] virtual core::String GetRevertText(core::StringView path) const = 0;

        /// Returns `path` and what is under it as text for the clipboard, or empty when it cannot be
        /// copied. The source chooses the format (the engine's serialization, say).
        [[nodiscard]] virtual core::String CopyValue(core::StringView path) const = 0;
        /// Reads `text` (from CopyValue) into `path` in each object, as one undo entry. Returns false,
        /// with `reason` filled in, when it does not fit.
        virtual bool PasteValue(core::StringView path, core::StringView text, core::String& reason) = 0;

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

    /// Returns `name` joined under `parent` ("position" and "x" give "position.x").
    [[nodiscard]] inline core::String JoinPropertyPath(core::StringView parent, core::StringView name)
    {
        core::String path(parent);
        if (path.Size() != 0)
            path.Append(core::StringView(u8"."));
        path.Append(name);
        return path;
    }
    /// Returns the parent path of `path` ("" for a top-level property).
    [[nodiscard]] inline core::StringView ParentPropertyPath(core::StringView path)
    {
        for (usize i = path.Size(); i > 0; --i)
            if (path[i - 1] == u8'.')
                return path.SubStr(0, i - 1);
        return core::StringView{};
    }

    /// Returns `write` applied to `old`. For flags, the bits outside its mask are kept.
    [[nodiscard]] inline Variant ApplyPropertyWrite(const Variant& old, const PropertyWrite& write)
    {
        if (write.Mask == ~u64{0})
            return write.Value;
        const u64 bits = (static_cast<u64>(old.AsInt()) & ~write.Mask) | (static_cast<u64>(write.Value.AsInt()) & write.Mask);
        return Variant(static_cast<i64>(bits));
    }

    /// A property's value over every object: the first object's value, whether any differs, and
    /// for flags, which bits differ.
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

    /// A source that holds its own values in memory: properties declared by path, each object's
    /// values, and lists of values (each object with its own elements). Each commit is reported to
    /// whoever keeps the undo, with the path and each object's value before and after; that owner
    /// restores values with SetValue. A list change is reported as the ListEdit that asked for it.
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
            : m_allocator(&allocator), m_properties(allocator), m_originals(allocator), m_defaults(allocator), m_objects(objects)
        {
        }

        /// Declares the property `path` with `value` for every object, and returns its info to fill
        /// in. Its name is the last segment, and it sits under its parent's path.
        PropertyInfo& Add(core::StringView path, PropertyKind kind, const Variant& value = {})
        {
            Entry entry(*m_allocator);
            entry.Path = core::String(path);
            const usize parent = ParentPropertyPath(path).Size();
            entry.Info.Name = core::String(parent == 0 ? path : path.SubStr(parent + 1, path.Size() - parent - 1));
            entry.Info.Kind = kind;
            for (usize i = 0; i < m_objects; ++i)
            {
                entry.Values.PushBack(value);
                entry.Elements.PushBack(core::Array<Variant>(*m_allocator));
            }
            m_properties.PushBack(core::Move(entry));
            NotifyPropertiesChanged();
            return m_properties.Back().Info;
        }
        /// Declares the list `path` of values of `elementKind`, with `count` elements in every object
        /// and `element` as a new element's value. Returns its info to fill in; by default the list
        /// may add, remove and move its elements.
        PropertyInfo& AddList(core::StringView path, PropertyKind elementKind, const Variant& element, usize count = 0)
        {
            PropertyInfo& info = Add(path, PropertyKind::List);
            info.ListOps = PropertyListAdd | PropertyListRemove | PropertyListMove;
            Entry& entry = m_properties.Back();
            entry.ElementKind = elementKind;
            entry.ElementDefault = element;
            for (core::Array<Variant>& elements : entry.Elements)
                for (usize i = 0; i < count; ++i)
                    elements.PushBack(element);
            return info;
        }
        /// Returns the info of `path` to change; call NotifyPropertiesChanged afterwards.
        [[nodiscard]] PropertyInfo* FindInfo(core::StringView path)
        {
            Entry* entry = Find(path);
            return entry != nullptr ? &entry->Info : nullptr;
        }
        /// Returns the info shared by every element of the list `path`, beyond its name, kind and
        /// read-only state (a range, a reference's type), to fill in. Returns null when `path` is not
        /// a list.
        [[nodiscard]] PropertyInfo* FindElementInfo(core::StringView path)
        {
            Entry* entry = Find(path);
            return entry != nullptr && entry->Info.Kind == PropertyKind::List ? &entry->ElementInfo : nullptr;
        }
        /// Gives every element of the list `path` a field `name` of `kind`, with `value` for new
        /// elements. From then on its elements are groups of their fields (`path.N.name`), as a
        /// reflected list's struct elements are. Declare fields before the list has elements.
        /// Returns the field's info to fill in (a range, a reference's type), or null when `path` is
        /// not a list.
        PropertyInfo* AddElementField(core::StringView path, core::StringView name, PropertyKind kind, const Variant& value = {})
        {
            Entry* list = Find(path);
            if (list == nullptr || list->Info.Kind != PropertyKind::List)
                return nullptr;
            list->ElementKind = PropertyKind::Group;
            ElementField field;
            field.Info.Name = core::String(name);
            field.Info.Kind = kind;
            field.Value = value;
            list->Fields.PushBack(core::Move(field));
            NotifyPropertiesChanged();
            return &list->Fields.Back().Info;
        }
        /// Tells the grid about changes made through FindInfo.
        void PropertiesChanged() { NotifyPropertiesChanged(); }

        /// Sets object `object`'s value at `path` (a property or an element) from code.
        void SetValue(core::StringView path, usize object, const Variant& value)
        {
            if (Variant* slot = Slot(path, object))
            {
                *slot = value;
                NotifyValuesChanged();
            }
        }
        /// Resizes object `object`'s list `path` to `count` elements from code; new elements take the
        /// list's new-element value.
        void ResizeList(core::StringView path, usize object, usize count)
        {
            Entry* list = Find(path);
            if (list == nullptr || list->Info.Kind != PropertyKind::List || object >= list->Elements.Size())
                return;
            core::Array<Variant>& elements = list->Elements[object];
            while (elements.Size() > count * list->Stride())
                elements.PopBack();
            while (elements.Size() < count * list->Stride())
                list->AppendElement(elements);
            NotifyPropertiesChanged();
        }
        /// Returns how many elements object `object`'s list `path` has.
        [[nodiscard]] usize ElementCount(core::StringView path, usize object) const
        {
            const Entry* entry = Find(path);
            return entry != nullptr && object < entry->Elements.Size() ? entry->Elements[object].Size() / entry->Stride() : 0;
        }

        /// Sets object `object`'s default at `path` (a property or an element), or every object's
        /// default with kAllObjects.
        static constexpr usize kAllObjects = ~usize{0};
        void SetDefault(core::StringView path, const Variant& value, usize object = kAllObjects)
        {
            for (Default& known : m_defaults)
                if (known.Path.AsView() == path && (object == kAllObjects || known.Object == object))
                    known.Value = value;
            for (usize i = 0; i < m_objects; ++i)
                if (object == kAllObjects || object == i)
                {
                    bool found = false;
                    for (const Default& known : m_defaults)
                        found = found || (known.Path.AsView() == path && known.Object == i);
                    if (!found)
                        m_defaults.PushBack(Default{core::String(path), i, value});
                }
            NotifyValuesChanged();
        }
        /// Called with the path of each revert, so it can be kept for undo.
        void SetOnReverted(core::Function<void(core::StringView)> onReverted) { m_onReverted = core::Move(onReverted); }

        /// Called with each commit, so it can be kept for undo.
        void SetOnCommit(core::Function<void(const Commit&)> onCommit) { m_onCommit = core::Move(onCommit); }
        /// Called on each preview of a gesture (a drag, a scrub, an open picker) as it goes, after
        /// the bag holds the previewed values. An owner that shows them live reads them then. The
        /// gesture ends in a commit or a cancel.
        void SetOnPreview(core::Function<void(core::StringView path)> onPreview) { m_onPreview = core::Move(onPreview); }
        /// Called when a gesture is cancelled (Escape); the values are then back as they were before
        /// it.
        void SetOnCancel(core::Function<void(core::StringView path)> onCancel) { m_onCancel = core::Move(onCancel); }
        /// Called with each list change made.
        void SetOnListEdited(core::Function<void(core::StringView, const ListEdit&)> onEdited) { m_onListEdited = core::Move(onEdited); }
        /// Returns how many edits have begun and not yet ended.
        [[nodiscard]] usize OpenEdits() const noexcept { return m_originals.Size(); }

        [[nodiscard]] usize ObjectCount() const override { return m_objects; }
        void GetProperties(core::StringView parent, core::Array<PropertyInfo>& out) const override
        {
            const usize first = out.Size();
            ListProperties(parent, out);
            // Let the owner describe each property last (a list element named for what it holds, a
            // closed group's summary line), as a reflected source's describe hook does.
            if (m_onDescribe)
                for (usize i = first; i < out.Size(); ++i)
                    m_onDescribe(JoinPropertyPath(parent, out[i].Name.AsView()).AsView(), out[i]);
        }
        /// Called for each row as it is listed, with its info to change. It supplies what the bag's
        /// entries cannot (a list element's name from its values, a closed group's summary line), and
        /// reads the values through GetValue.
        void SetOnDescribe(core::Function<void(core::StringView path, PropertyInfo& info)> onDescribe) { m_onDescribe = core::Move(onDescribe); }

    private:
        void ListProperties(core::StringView parent, core::Array<PropertyInfo>& out) const
        {
            // A list shows as many elements as the object with the fewest has.
            if (const Entry* list = Find(parent); list != nullptr && list->Info.Kind == PropertyKind::List)
            {
                usize count = ~usize{0};
                for (const core::Array<Variant>& elements : list->Elements)
                    count = core::Min(count, elements.Size() / list->Stride());
                for (usize i = 0; i < count && i != ~usize{0}; ++i)
                {
                    PropertyInfo info = list->ElementInfo;
                    info.Name = core::Format(u8"{}", i);
                    info.Kind = list->ElementKind;
                    info.ReadOnly = list->Info.ReadOnly;
                    out.PushBack(core::Move(info));
                }
                return;
            }
            // The fields of a group element ("variants.2" has its clip and its weight).
            if (const Entry* list = Find(ParentPropertyPath(parent)); list != nullptr && list->Info.Kind == PropertyKind::List && !list->Fields.IsEmpty())
            {
                for (const ElementField& field : list->Fields)
                {
                    PropertyInfo info = field.Info;
                    info.ReadOnly = info.ReadOnly || list->Info.ReadOnly;
                    out.PushBack(core::Move(info));
                }
                return;
            }
            for (const Entry& entry : m_properties)
                if (ParentPropertyPath(entry.Path.AsView()) == parent)
                    out.PushBack(entry.Info);
        }

    public:
        [[nodiscard]] Variant GetValue(core::StringView path, usize object) const override
        {
            if (const Entry* list = Find(path); list != nullptr && list->Info.Kind == PropertyKind::List)
                return object < list->Elements.Size() ? Variant(static_cast<i64>(list->Elements[object].Size() / list->Stride())) : Variant{};
            const Variant* slot = const_cast<PropertyBag*>(this)->Slot(path, object);
            return slot != nullptr ? *slot : Variant{};
        }

        void BeginEdit(core::StringView path) override
        {
            if (FindOriginal(path) != nullptr || Slot(path, 0) == nullptr)
                return;
            Original original{core::String(path), core::Array<Variant>(*m_allocator)};
            for (usize i = 0; i < m_objects; ++i)
                original.Values.PushBack(Read(path, i));
            m_originals.PushBack(core::Move(original));
        }
        void PreviewEdit(core::StringView path, const PropertyWrite& write) override
        {
            if (IsReadOnly(path))
                return;
            for (usize i = 0; i < m_objects; ++i)
                if (Variant* slot = Slot(path, i))
                    *slot = ApplyPropertyWrite(*slot, write);
            NotifyValuesChanged();
            if (m_onPreview)
                m_onPreview(path);
        }
        void CommitEdit(core::StringView path, const PropertyWrite& write) override
        {
            if (IsReadOnly(path) || Slot(path, 0) == nullptr)
            {
                EndEdit(path);
                return;
            }
            Commit commit{core::String(path), core::Array<Variant>(*m_allocator), core::Array<Variant>(*m_allocator)};
            const Original* original = FindOriginal(path);
            for (usize i = 0; i < m_objects; ++i)
            {
                Variant* slot = Slot(path, i);
                const Variant before = original != nullptr && i < original->Values.Size() ? original->Values[i] : (slot != nullptr ? *slot : Variant{});
                commit.Before.PushBack(before);
                if (slot != nullptr)
                    *slot = ApplyPropertyWrite(before, write); // apply to the original value, not to the previews
                commit.After.PushBack(slot != nullptr ? *slot : Variant{});
            }
            EndEdit(path);
            NotifyValuesChanged();
            if (m_onCommit)
                m_onCommit(commit);
        }
        void CancelEdit(core::StringView path) override
        {
            if (const Original* original = FindOriginal(path))
                for (usize i = 0; i < m_objects && i < original->Values.Size(); ++i)
                    if (Variant* slot = Slot(path, i))
                        *slot = original->Values[i];
            EndEdit(path);
            NotifyValuesChanged();
            if (m_onCancel)
                m_onCancel(path);
        }
        [[nodiscard]] Variant GetDefault(core::StringView path, usize object) const override
        {
            for (const Default& known : m_defaults)
                if (known.Object == object && known.Path.AsView() == path)
                    return known.Value;
            return Variant{};
        }
        bool Revert(core::StringView path) override
        {
            bool changed = false;
            for (const Default& known : m_defaults)
            {
                const core::StringView at = known.Path.AsView();
                const bool under = at == path || (at.Size() > path.Size() && at.SubStr(0, path.Size()) == path && at[path.Size()] == u8'.');
                Variant* slot = under ? Slot(at, known.Object) : nullptr;
                if (slot != nullptr && (slot->GetType() != known.Value.GetType() || slot->Compare(known.Value) != 0))
                {
                    *slot = known.Value;
                    changed = true;
                }
            }
            if (!changed)
                return false;
            NotifyValuesChanged();
            if (m_onReverted)
                m_onReverted(path);
            return true;
        }
        [[nodiscard]] core::String GetRevertText(core::StringView) const override { return core::String(u8"Revert to default"); }

        // Copies a single value as text, taking the first object's; a group or a list is not copied
        // here.
        [[nodiscard]] core::String CopyValue(core::StringView path) const override
        {
            const Variant* slot = const_cast<PropertyBag*>(this)->Slot(path, 0);
            return slot != nullptr ? slot->ToString() : core::String{};
        }
        bool PasteValue(core::StringView path, core::StringView text, core::String& reason) override
        {
            const PropertyKind kind = KindAt(path);
            Variant value;
            switch (kind)
            {
            case PropertyKind::Bool:
                if (text == core::StringView(u8"true") || text == core::StringView(u8"false"))
                    value = Variant(text == core::StringView(u8"true"));
                break;
            case PropertyKind::Integer:
            case PropertyKind::Enum:
            case PropertyKind::Flags:
                if (const Optional<i64> number = core::ParseInt(core::Trim(text)); number.HasValue())
                    value = Variant(number.Value());
                break;
            case PropertyKind::Float:
                if (const Optional<f64> number = core::ParseFloat(core::Trim(text)); number.HasValue())
                    value = Variant(number.Value());
                break;
            case PropertyKind::String:
                value = Variant(text);
                break;
            case PropertyKind::Reference:
            {
                Guid id;
                if (core::Trim(text).IsEmpty() || Guid::TryParse(core::Trim(text), id))
                    value = Variant(core::Trim(text));
                break;
            }
            case PropertyKind::Color:
                if (const Optional<Color> color = ParseColor(core::Trim(text)); color.HasValue())
                    value = Variant(color.Value());
                break;
            default:
                break;
            }
            if (value.IsEmpty() || IsReadOnly(path) || Slot(path, 0) == nullptr)
            {
                reason = core::Format(u8"\"{}\" is no {}", text, PropertyKindLabel(kind));
                return false;
            }
            BeginEdit(path);
            CommitEdit(path, PropertyWrite{value});
            return true;
        }

        bool EditList(core::StringView path, const ListEdit& edit) override
        {
            Entry* list = Find(path);
            if (list == nullptr || list->Info.Kind != PropertyKind::List || list->Info.ReadOnly)
                return false;
            const u8 needs = edit.Kind == ListEdit::Op::Insert ? PropertyListAdd : (edit.Kind == ListEdit::Op::Remove ? PropertyListRemove : PropertyListMove);
            if ((list->Info.ListOps & needs) == 0)
                return false;
            bool changed = false;
            const usize stride = list->Stride(); // the values per element: one, or one per group field
            for (core::Array<Variant>& elements : list->Elements)
            {
                const usize count = elements.Size() / stride;
                switch (edit.Kind)
                {
                case ListEdit::Op::Insert:
                    if (list->Info.MaxCount == 0 || count < list->Info.MaxCount)
                    {
                        core::Array<Variant> element(*m_allocator);
                        list->AppendElement(element);
                        const usize at = core::Min(edit.Index, count) * stride;
                        for (usize f = 0; f < stride; ++f)
                            elements.Insert(at + f, element[f]);
                        changed = true;
                    }
                    break;
                case ListEdit::Op::Remove:
                    if (edit.Index < count)
                    {
                        for (usize f = 0; f < stride; ++f)
                            elements.RemoveAt(edit.Index * stride);
                        changed = true;
                    }
                    break;
                case ListEdit::Op::Move:
                    if (edit.Index < count && edit.To < count && edit.Index != edit.To)
                    {
                        core::Array<Variant> moved(*m_allocator);
                        for (usize f = 0; f < stride; ++f)
                            moved.PushBack(elements[edit.Index * stride + f]);
                        for (usize f = 0; f < stride; ++f)
                            elements.RemoveAt(edit.Index * stride);
                        for (usize f = 0; f < stride; ++f)
                            elements.Insert(edit.To * stride + f, moved[f]);
                        changed = true;
                    }
                    break;
                }
            }
            if (!changed)
                return false;
            NotifyPropertiesChanged();
            if (m_onListEdited)
                m_onListEdited(path, edit);
            return true;
        }

    private:
        // A field of a group element: its info (name, kind and the rest) and its value in a new
        // element.
        struct ElementField
        {
            PropertyInfo Info;
            Variant Value;
        };
        struct Entry
        {
            explicit Entry(core::IAllocator& allocator) : Values(allocator), Elements(allocator), Fields(allocator) {}
            core::String Path;
            PropertyInfo Info;
            core::Array<Variant> Values;                 // one value per object
            core::Array<core::Array<Variant>> Elements;  // for a list, each object's elements, with a group's fields one after another
            PropertyKind ElementKind = PropertyKind::Float;
            Variant ElementDefault;
            PropertyInfo ElementInfo; // for a list, the info each element has beyond its name and kind
            core::Array<ElementField> Fields; // for a list of groups, each element's fields, in order

            /// Returns how many values make one element: one, or one per group field.
            [[nodiscard]] usize Stride() const noexcept { return Fields.IsEmpty() ? 1u : Fields.Size(); }
            /// Appends a new element's values to `elements`.
            void AppendElement(core::Array<Variant>& elements) const
            {
                if (Fields.IsEmpty())
                    elements.PushBack(ElementDefault);
                for (const ElementField& field : Fields)
                    elements.PushBack(field.Value);
            }
        };
        struct Original
        {
            core::String Path;
            core::Array<Variant> Values;
        };
        struct Default
        {
            core::String Path;
            usize Object = 0;
            Variant Value;
        };

        const Entry* Find(core::StringView path) const
        {
            for (const Entry& entry : m_properties)
                if (entry.Path.AsView() == path)
                    return &entry;
            return nullptr;
        }
        Entry* Find(core::StringView path) { return const_cast<Entry*>(static_cast<const PropertyBag*>(this)->Find(path)); }
        // Returns object `object`'s value at `path`: a property, or an element of a list ("tags.2").
        Variant* Slot(core::StringView path, usize object)
        {
            if (object >= m_objects)
                return nullptr;
            if (Entry* entry = Find(path))
                return entry->Info.Kind == PropertyKind::List ? nullptr : &entry->Values[object];
            const core::StringView parent = ParentPropertyPath(path);
            Entry* list = parent.Size() != 0 ? Find(parent) : nullptr;
            if (list != nullptr && list->Info.Kind == PropertyKind::List)
            {
                if (!list->Fields.IsEmpty())
                    return nullptr; // a group element has no value of its own; only its fields do
                const Optional<i64> index = core::ParseInt(path.SubStr(parent.Size() + 1, path.Size() - parent.Size() - 1));
                core::Array<Variant>& elements = list->Elements[object];
                return index.HasValue() && index.Value() >= 0 && static_cast<usize>(index.Value()) < elements.Size() ? &elements[static_cast<usize>(index.Value())] : nullptr;
            }
            // A group element's field ("variants.2.weight").
            const core::StringView element = parent;
            const core::StringView listPath = ParentPropertyPath(element);
            list = listPath.Size() != 0 ? Find(listPath) : nullptr;
            if (list == nullptr || list->Info.Kind != PropertyKind::List || list->Fields.IsEmpty())
                return nullptr;
            const Optional<i64> index = core::ParseInt(element.SubStr(listPath.Size() + 1, element.Size() - listPath.Size() - 1));
            const core::StringView name = path.SubStr(element.Size() + 1, path.Size() - element.Size() - 1);
            for (usize f = 0; f < list->Fields.Size(); ++f)
                if (list->Fields[f].Info.Name.AsView() == name)
                {
                    core::Array<Variant>& elements = list->Elements[object];
                    const usize at = index.HasValue() && index.Value() >= 0 ? static_cast<usize>(index.Value()) * list->Stride() + f : elements.Size();
                    return at < elements.Size() ? &elements[at] : nullptr;
                }
            return nullptr;
        }
        [[nodiscard]] Variant Read(core::StringView path, usize object)
        {
            const Variant* slot = Slot(path, object);
            return slot != nullptr ? *slot : Variant{};
        }
        [[nodiscard]] PropertyKind KindAt(core::StringView path) const
        {
            if (const Entry* entry = Find(path))
                return entry->Info.Kind;
            const Entry* list = Find(ParentPropertyPath(path));
            if (list != nullptr && list->Info.Kind == PropertyKind::List)
                return list->ElementKind;
            // A group element's field has its own kind.
            const core::StringView element = ParentPropertyPath(path);
            if (const Entry* outer = Find(ParentPropertyPath(element)); outer != nullptr && outer->Info.Kind == PropertyKind::List)
                for (const ElementField& field : outer->Fields)
                    if (field.Info.Name.AsView() == path.SubStr(element.Size() + 1, path.Size() - element.Size() - 1))
                        return field.Info.Kind;
            return PropertyKind::Group;
        }
        static core::StringView PropertyKindLabel(PropertyKind kind) noexcept
        {
            switch (kind)
            {
            case PropertyKind::Bool:
                return core::StringView(u8"bool");
            case PropertyKind::Integer:
            case PropertyKind::Enum:
            case PropertyKind::Flags:
                return core::StringView(u8"integer");
            case PropertyKind::Float:
                return core::StringView(u8"number");
            case PropertyKind::String:
                return core::StringView(u8"text");
            case PropertyKind::Color:
                return core::StringView(u8"colour");
            case PropertyKind::Reference:
                return core::StringView(u8"guid");
            default:
                return core::StringView(u8"value it can take");
            }
        }
        [[nodiscard]] bool IsReadOnly(core::StringView path) const
        {
            if (const Entry* entry = Find(path))
                return entry->Info.ReadOnly;
            const Entry* list = Find(ParentPropertyPath(path));
            if (list != nullptr)
                return list->Info.ReadOnly;
            const Entry* outer = Find(ParentPropertyPath(ParentPropertyPath(path))); // a group element's field
            return outer != nullptr && outer->Info.Kind == PropertyKind::List && outer->Info.ReadOnly;
        }
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

        core::IAllocator* m_allocator;
        core::Array<Entry> m_properties;
        core::Array<Original> m_originals;
        core::Array<Default> m_defaults;
        usize m_objects;
        core::Function<void(const Commit&)> m_onCommit;
        core::Function<void(core::StringView)> m_onPreview;
        core::Function<void(core::StringView)> m_onCancel;
        core::Function<void(core::StringView, const ListEdit&)> m_onListEdited;
        core::Function<void(core::StringView, PropertyInfo&)> m_onDescribe; // SetOnDescribe
        core::Function<void(core::StringView)> m_onReverted;
    };
}
