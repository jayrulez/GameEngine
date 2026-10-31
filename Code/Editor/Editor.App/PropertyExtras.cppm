// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :property_extras partition
//
// Rows an editor's grid shows besides values, each a property of a source the editor fills (a
// PropertyBag), chosen by its hint:
//   - a notice (`notice`): its value is what it says, wrapped, styled as a warning (a rigid body
//     with a cooked shape and no collision shape: it has no collider);
//   - an action (`action`): a button its value names; a click commits the row as it is, so whoever
//     keeps the source runs the action on that commit (a zone's Bake).
// They live in the editor because only the editor uses them; they move to GUI.Toolkit if something
// else needs them.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:property_extras;

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// The two editors' hints at namespace scope, for another library to read: MSVC does not export
    /// a class's static constexpr member from a shared library, so a use elsewhere does not link.
    inline constexpr StringView kNoticeHint = u8"notice";
    inline constexpr StringView kActionHint = u8"action";

    class NoticePropertyEditor final : public gui::SingleControlEditor
    {
        RTTI_OBJECT(NoticePropertyEditor, gui::SingleControlEditor)
    public:
        static constexpr StringView kHint = kNoticeHint;

        NoticePropertyEditor()
        {
            AddClass(u8"notice");
            m_text = MakeRef<gui::Label>(MemoryAllocator());
            m_text->AddClass(u8"notice-text");
            Hold(*m_text);
        }
        [[nodiscard]] StringView Text() const { return m_text->GetText(); }

    protected:
        void OnBind(const gui::PropertyInfo& info, const gui::PropertyValue& value) override
        {
            (void)info;
            m_text->SetText(value.Value.AsString());
        }

    private:
        RefPtr<gui::Label> m_text;
    };

    class ActionPropertyEditor final : public gui::SingleControlEditor
    {
        RTTI_OBJECT(ActionPropertyEditor, gui::SingleControlEditor)
    public:
        static constexpr StringView kHint = kActionHint;

        ActionPropertyEditor()
        {
            AddClass(u8"action");
            m_button = MakeRef<gui::Button>(MemoryAllocator());
            ActionPropertyEditor* self = this;
            m_button->SetOnClick([self]() { self->Run(); });
            Hold(*m_button);
        }
        [[nodiscard]] gui::Button* Button() const noexcept { return m_button.Get(); }
        /// Runs the action as a click does, by committing the row as it is.
        void Run()
        {
            RefPtr<ActionPropertyEditor> keep(this); // keep alive: the action may rebuild the grid
            if (!IsReadOnly())
                ApplyEdit(gui::PropertyWrite{gui::Variant(m_label.AsView())});
        }

    protected:
        void OnBind(const gui::PropertyInfo& info, const gui::PropertyValue& value) override
        {
            m_label = String(value.Value.AsString());
            m_button->SetText(m_label.AsView());
            m_button->SetTooltip(info.Description.AsView());
            m_button->SetEnabled(!IsReadOnly());
        }

    private:
        RefPtr<gui::Button> m_button;
        String m_label;
    };

    // A struct of static functions rather than a function: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct PropertyExtras
    {
        /// Registers the notice and action editors in `registry`'s grid, by their hints.
        static void Register(gui::PropertyEditorRegistry& registry)
        {
            registry.RegisterHint(NoticePropertyEditor::kHint, [](IAllocator& allocator) -> RefPtr<gui::PropertyEditor> { return MakeRef<NoticePropertyEditor>(allocator); });
            registry.RegisterHint(ActionPropertyEditor::kHint, [](IAllocator& allocator) -> RefPtr<gui::PropertyEditor> { return MakeRef<ActionPropertyEditor>(allocator); });
        }
    };

    RTTI_DEFINE_OBJECT(NoticePropertyEditor, "rtti::editor::editor::app")
    RTTI_DEFINE_OBJECT(ActionPropertyEditor, "rtti::editor::editor::app")
}
