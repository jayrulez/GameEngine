// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_page partition
//
// What every asset page shares (Documentation/Specs/editor-gui.md, 5.4, layer 1), so each of the
// twenty libraries does not write its own:
//   - AssetPageServices: what a page asks of the editor when it is made (its markup, the asset
//     slots' table, the type icons), filled by the application.
//   - FindById: an element of a page's markup by its id, which is how code binds to markup.
//   - XmlPropertyTextFormat: the engine's XmlSerializer as a reflected source's text format, what
//     an edit's before and after are written in and an undo reads back (the scene's inspector's
//     until now).
//   - ReflectedEditCommand: one reflected source's change, undone and redone by its text on the
//     object it landed in, found again each time (the object may have been read anew since).

module;
#include "Core/Prelude.h"

export module editor.app:asset_page;

import foundation.core;
import foundation.xml;
import foundation.xml.serialization;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import :resource_ref_editor;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    struct AssetPageServices
    {
        /// A page's markup by its path under Data/ (null: it could not be read).
        Function<RefPtr<gui::Node>(StringView path)> loadMarkup;
        const ResourceRefServices* refs = nullptr; // the asset slots' (null: none)
        editor::EditorIcons* icons = nullptr;
    };

    /// The element under `root` (itself included) whose id is `id`, as a T; null when there is
    /// none or it is not one.
    template <typename T>
    [[nodiscard]] T* FindById(gui::Node& root, StringView id)
    {
        Array<gui::Node*> pending(EditorRootAllocator());
        pending.PushBack(&root);
        while (!pending.IsEmpty())
        {
            gui::Node* node = pending.Back();
            pending.PopBack();
            if (auto* widget = Cast<gui::UIWidget>(node); widget != nullptr && widget->GetId() == id)
                return Cast<T>(node);
            for (usize i = 0; i < node->ChildCount(); ++i)
                pending.PushBack(node->GetChildAt(i));
        }
        return nullptr;
    }

    class XmlPropertyTextFormat final : public gui::IPropertyTextFormat
    {
    public:
        explicit XmlPropertyTextFormat(IAllocator& allocator) : m_allocator(allocator) {}

        String Write(Function<void(ISerializer&)> write) override
        {
            foundation::xml::XmlSerializer ar(m_allocator);
            write(ar);
            String text;
            ar.GetOutput(text);
            return ar.IsOk() ? text : String{};
        }
        bool Read(StringView text, Function<void(ISerializer&)> read) override
        {
            foundation::xml::XmlDocument doc(m_allocator);
            if (doc.Parse(text) != foundation::xml::XmlResult::Ok)
                return false;
            foundation::xml::XmlSerializer ar(doc);
            read(ar);
            return ar.IsOk();
        }

    private:
        IAllocator& m_allocator;
    };

    class ReflectedEditCommand final : public editor::IEditorCommand
    {
    public:
        /// `target` finds the object of `type` the change landed in (null: gone, and the step does
        /// nothing); `applied`, when given, runs after each restore (a reference bound again, a
        /// preview told). `format` outlives the command.
        ReflectedEditCommand(const TypeInfo& type, Function<void*()> target, gui::ReflectedChange change, gui::IPropertyTextFormat& format,
                             StringView typeId, Function<void()> applied = {})
            : m_type(&type), m_target(Move(target)), m_change(Move(change)), m_format(&format), m_typeId(typeId), m_applied(Move(applied))
        {
        }

        [[nodiscard]] bool Execute() override { return Restore(false); }
        void Undo() override { (void)Restore(true); }
        [[nodiscard]] StringView TypeId() const override { return m_typeId.AsView(); }

    private:
        bool Restore(bool before)
        {
            void* values = m_target ? m_target() : nullptr;
            if (values == nullptr)
                return false;
            gui::ReflectedPropertySource source(EditorRootAllocator());
            source.SetTextFormat(m_format);
            const Instance object(values, m_type);
            source.SetObjects(Span<const Instance>(&object, 1u));
            const bool ok = source.Restore(m_change, before);
            if (m_applied)
                m_applied();
            return ok;
        }

        const TypeInfo* m_type;
        Function<void*()> m_target;
        gui::ReflectedChange m_change;
        gui::IPropertyTextFormat* m_format;
        String m_typeId;
        Function<void()> m_applied;
    };
}
