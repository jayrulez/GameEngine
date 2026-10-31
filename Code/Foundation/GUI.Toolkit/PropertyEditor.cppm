// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :property_editor partition
//
// A property grid's editors. An editor is the value part of a row and is bound to one property at
// a time; rows are pooled, so an editor shows one property and later another. It edits through its
// host in gestures, which begin, preview, and then commit or cancel. The registry chooses an editor
// in layers: by the property's hint, then by its kind, and otherwise a row that says nothing fits.
// An application can register its own editors.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui.toolkit:property_editor;

import foundation.core;
import foundation.gui;
import :property_source;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// What an editor edits through: its grid, which tracks the gesture and forwards it to the
    /// source.
    class IPropertyEditHost
    {
    public:
        virtual ~IPropertyEditHost() = default;
        virtual void BeginPropertyEdit(core::StringView path) = 0;
        virtual void PreviewPropertyEdit(core::StringView path, const PropertyWrite& write) = 0;
        virtual void CommitPropertyEdit(core::StringView path, const PropertyWrite& write) = 0;
        virtual void CancelPropertyEdit(core::StringView path) = 0;
        /// Changes a list (an insert, a removal or a move). Each call is one undo entry.
        virtual bool EditPropertyList(core::StringView path, const ListEdit& edit) = 0;
        /// Fills `out` with the properties under `path` and their values. An editor that covers its
        /// children shows these.
        virtual void GetChildProperties(core::StringView path, core::Array<PropertyInfo>& out) const = 0;
        [[nodiscard]] virtual PropertyValue ReadPropertyValue(core::StringView path, PropertyKind kind) const = 0;
    };

    /// The value part of a property's row.
    class PropertyEditor : public UIWidget
    {
        RTTI_OBJECT(PropertyEditor, UIWidget)
    public:
        PropertyEditor() { SetTag(core::StringView(u8"propertyeditor")); }

        /// Shows `info`'s property at `path`, whose value over the objects is `value`.
        void Bind(IPropertyEditHost& host, core::StringView path, const PropertyInfo& info, const PropertyValue& value)
        {
            m_host = &host;
            m_path = core::String(path);
            m_readOnly = info.ReadOnly;
            OnBind(info, value);
        }
        [[nodiscard]] core::StringView GetPath() const noexcept { return m_path.AsView(); }
        [[nodiscard]] bool IsReadOnly() const noexcept { return m_readOnly; }
        /// Returns whether the editor is being typed into. While it is, the grid leaves what it shows
        /// alone as the values refresh, because a typed value belongs to the edit until Return or
        /// losing focus commits it.
        [[nodiscard]] virtual bool IsTyping() const { return false; }

    protected:
        [[nodiscard]] IPropertyEditHost* GetHost() const noexcept { return m_host; }

        /// Shows a property and its value. A read-only editor shows the value but does not edit it.
        virtual void OnBind(const PropertyInfo& info, const PropertyValue& value) = 0;

        // The gesture, sent through the host. Apply begins and commits an edit at once (for a click).
        void BeginEdit()
        {
            if (m_host != nullptr && !m_readOnly)
                m_host->BeginPropertyEdit(m_path.AsView());
        }
        void PreviewEdit(const PropertyWrite& write)
        {
            if (m_host != nullptr && !m_readOnly)
                m_host->PreviewPropertyEdit(m_path.AsView(), write);
        }
        void CommitEdit(const PropertyWrite& write)
        {
            if (m_host != nullptr && !m_readOnly)
                m_host->CommitPropertyEdit(m_path.AsView(), write);
        }
        void CancelEdit()
        {
            if (m_host != nullptr && !m_readOnly)
                m_host->CancelPropertyEdit(m_path.AsView());
        }
        void ApplyEdit(const PropertyWrite& write)
        {
            BeginEdit();
            CommitEdit(write);
        }
        bool EditList(const ListEdit& edit) { return m_host != nullptr && !m_readOnly && m_host->EditPropertyList(m_path.AsView(), edit); }

    private:
        IPropertyEditHost* m_host = nullptr;
        core::String m_path;
        bool m_readOnly = false;
    };

    /// Returns a kind's name, as the fallback row shows it.
    [[nodiscard]] inline core::StringView PropertyKindName(PropertyKind kind) noexcept
    {
        switch (kind)
        {
        case PropertyKind::Group:
            return core::StringView(u8"group");
        case PropertyKind::Bool:
            return core::StringView(u8"bool");
        case PropertyKind::Integer:
            return core::StringView(u8"integer");
        case PropertyKind::Float:
            return core::StringView(u8"float");
        case PropertyKind::String:
            return core::StringView(u8"string");
        case PropertyKind::Enum:
            return core::StringView(u8"enum");
        case PropertyKind::Flags:
            return core::StringView(u8"flags");
        case PropertyKind::List:
            return core::StringView(u8"list");
        case PropertyKind::Color:
            return core::StringView(u8"colour");
        case PropertyKind::Curve:
            return core::StringView(u8"curve");
        case PropertyKind::Gradient:
            return core::StringView(u8"gradient");
        case PropertyKind::Reference:
            return core::StringView(u8"reference");
        }
        return core::StringView(u8"value");
    }

    /// The row of a property that no editor takes. It says so, naming the hint or the kind it
    /// could not fit.
    class UnknownPropertyEditor : public PropertyEditor
    {
        RTTI_OBJECT(UnknownPropertyEditor, PropertyEditor)
    public:
        UnknownPropertyEditor()
        {
            AddClass(core::StringView(u8"unknown"));
            m_label = core::MakeRef<Label>(MemoryAllocator());
            m_label->SetTextAlignment(TextHAlign::Start, TextVAlign::Middle);
            m_label->SetHitTestVisible(false);
            AddChild(m_label.Get());
        }
        [[nodiscard]] core::StringView GetMessage() const { return m_label->GetText(); }

    protected:
        void OnBind(const PropertyInfo& info, const PropertyValue&) override
        {
            m_label->SetText(core::Format(u8"No editor for {}", info.Hint.Size() != 0 ? info.Hint.AsView() : PropertyKindName(info.Kind)).AsView());
        }
        void OnLayout() override { m_label->Arrange(GetContentBounds()); }
        void OnTextStyleChange() override { ShareTextStyle(*m_label); }

    private:
        core::RefPtr<Label> m_label;
    };

    using PropertyEditorFactory = core::Function<core::RefPtr<PropertyEditor>(core::IAllocator&)>;

    /// PropertyEditorRegistry::kNoEditor, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr u32 kPropertyEditorRegistryNoEditor = 0;

    /// The editors a grid chooses from. Each registered editor has its own row type (and so its own
    /// pool), numbered from 1; 0 is the row for properties no editor takes.
    class PropertyEditorRegistry
    {
    public:
        static constexpr u32 kNoEditor = kPropertyEditorRegistryNoEditor;

        PropertyEditorRegistry() = default;
        explicit PropertyEditorRegistry(core::IAllocator& allocator) : m_entries(allocator) {}

        /// Registers the editor for properties hinted `hint`, chosen before their kind's editor. It
        /// replaces any editor already registered for the hint. An editor registered with
        /// `coversChildren` shows what is under the property itself, so its row has no chevron and
        /// no rows under it (a Float3's three fields on one row).
        void RegisterHint(core::StringView hint, PropertyEditorFactory factory, bool coversChildren = false)
        {
            Register(core::String(hint), false, PropertyKind::Group, core::Move(factory), coversChildren);
        }
        /// Registers the editor for properties of `kind` that no hint takes, replacing any editor
        /// already registered for the kind.
        void RegisterKind(PropertyKind kind, PropertyEditorFactory factory) { Register(core::String{}, true, kind, core::Move(factory), false); }

        /// Returns the editor type for `info`, chosen by its hint and then its kind, or kNoEditor when
        /// none fits.
        [[nodiscard]] u32 Choose(const PropertyInfo& info) const
        {
            if (info.Hint.Size() != 0)
                for (usize i = 0; i < m_entries.Size(); ++i)
                    if (!m_entries[i].ByKind && m_entries[i].Hint == info.Hint)
                        return static_cast<u32>(i + 1);
            for (usize i = 0; i < m_entries.Size(); ++i)
                if (m_entries[i].ByKind && m_entries[i].Kind == info.Kind)
                    return static_cast<u32>(i + 1);
            return kNoEditor;
        }
        /// Returns whether the editor of `type` shows what is under its property.
        [[nodiscard]] bool CoversChildren(u32 type) const noexcept { return type != kNoEditor && type <= m_entries.Size() && m_entries[type - 1].CoversChildren; }
        /// Makes a new editor of `type`, as returned by Choose.
        [[nodiscard]] core::RefPtr<PropertyEditor> Create(u32 type, core::IAllocator& allocator) const
        {
            if (type != kNoEditor && type <= m_entries.Size() && m_entries[type - 1].Factory)
                if (core::RefPtr<PropertyEditor> made = m_entries[type - 1].Factory(allocator))
                    return made;
            return core::MakeRef<UnknownPropertyEditor>(allocator);
        }
        /// Returns a count of registry changes. A grid whose pools were made before a change rebuilds
        /// them.
        [[nodiscard]] u32 GetGeneration() const noexcept { return m_generation; }

    private:
        struct Entry
        {
            core::String Hint;
            bool ByKind = false;
            PropertyKind Kind = PropertyKind::Group;
            PropertyEditorFactory Factory;
            bool CoversChildren = false;
        };
        void Register(core::String hint, bool byKind, PropertyKind kind, PropertyEditorFactory factory, bool coversChildren)
        {
            ++m_generation;
            for (Entry& entry : m_entries)
                if (entry.ByKind == byKind && (byKind ? entry.Kind == kind : entry.Hint == hint))
                {
                    entry.Factory = core::Move(factory);
                    entry.CoversChildren = coversChildren;
                    return;
                }
            m_entries.PushBack(Entry{core::Move(hint), byKind, kind, core::Move(factory), coversChildren});
        }

        core::Array<Entry> m_entries;
        u32 m_generation = 0;
    };
}
