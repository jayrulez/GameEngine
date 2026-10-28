// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :bone_name_editor partition.
//
// The inspector's bone name (Documentation/Specs/editor-gui.md, 5.6, gap 1): a string with the
// `boneName` attribute (an IK chain's bones, an aim's) picked from the bones of the animator at or
// above the entity shown, so a chain is chosen, not typed. "(none)" first, then the bones in the
// skeleton's order; a name the skeleton does not have stays listed and says so (the component
// turns itself off for it while the scene runs). With no animator, or its skeleton not loaded, the
// plain text field. A pick is one write through the grid, so one undo step.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.scene:bone_name_editor;

import foundation.core;
import foundation.scene;
import foundation.animation;
import foundation.gui;
import foundation.gui.toolkit;
import engine.animation;
import :edit;

using namespace foundation::core;

export namespace editor
{
    class BoneNameEditor final : public foundation::gui::PropertyEditor
    {
        RTTI_OBJECT(BoneNameEditor, foundation::gui::PropertyEditor)
    public:
        /// The attribute, and the hint the reflected source gives its rows.
        static constexpr StringView kHint = u8"boneName";
        static constexpr StringView kNoneText = u8"(none)";

        /// `edit` outlives the editor (the registrar's).
        explicit BoneNameEditor(SceneEditContext* edit) : m_edit(edit), m_names(MemoryAllocator())
        {
            AddClass(u8"bone-name");
            BoneNameEditor* self = this;
            m_combo = MakeRef<foundation::gui::ComboBox>(MemoryAllocator());
            m_combo->SetPlaceholder(String(foundation::gui::MixedValueText()));
            m_combo->SetOnSelectionChanged([self](i32 index)
            {
                if (self->m_binding || index < 0 || static_cast<usize>(index) >= self->m_names.Size())
                    return;
                self->ApplyEdit(foundation::gui::PropertyWrite{foundation::gui::Variant(self->m_names[static_cast<usize>(index)].AsView())});
            });
            m_field = MakeRef<foundation::gui::TextField>(MemoryAllocator());
            m_field->SetOnTextChanged([self](StringView text) { self->ApplyEdit(foundation::gui::PropertyWrite{foundation::gui::Variant(text)}); });
            AddChild(m_combo.Get());
            AddChild(m_field.Get());
        }
        BoneNameEditor() : BoneNameEditor(nullptr) {} // what RTTI makes: the text field alone

        /// The editor for every `boneName` row `registry` shows, over `edit`'s scene.
        static void Register(foundation::gui::PropertyEditorRegistry& registry, SceneEditContext* edit)
        {
            registry.RegisterHint(kHint, [edit](IAllocator& allocator) -> RefPtr<foundation::gui::PropertyEditor>
                                  { return MakeRef<BoneNameEditor>(allocator, edit); });
        }

        [[nodiscard]] bool IsTyping() const override { return m_field->IsFocused(); }
        [[nodiscard]] foundation::gui::ComboBox* GetComboBox() const noexcept { return m_combo.Get(); }
        [[nodiscard]] foundation::gui::TextField* GetField() const noexcept { return m_field.Get(); }
        /// True while it lists a skeleton's bones; false for the text field.
        [[nodiscard]] bool ListsBones() const noexcept { return m_listing; }

    protected:
        void OnBind(const foundation::gui::PropertyInfo& info, const foundation::gui::PropertyValue& value) override
        {
            (void)info;
            m_binding = true;
            const String current = value.Mixed ? String{} : String(value.Value.AsString());
            const engine::animation::IkAuthoringAnimator animator = Animator();
            m_listing = animator.skeleton != nullptr;
            m_combo->SetVisible(m_listing);
            m_field->SetVisible(!m_listing);
            if (m_listing)
            {
                m_names.Clear();
                m_names.PushBack(String());
                m_combo->SetItemsText(String{});
                m_combo->AddItem(kNoneText);
                for (i32 i = 0; i < animator.skeleton->BoneCount(); ++i)
                {
                    m_names.PushBack(animator.skeleton->GetBone(i)->name);
                    m_combo->AddItem(m_names.Back().AsView());
                }
                if (!current.IsEmpty() && animator.skeleton->FindBone(current.AsView()) < 0)
                {
                    m_names.PushBack(current);
                    m_combo->AddItem(Format(u8"{} (not in the skeleton)", current).AsView());
                }
                i32 selected = -1;
                if (!value.Mixed)
                    for (usize i = 0; i < m_names.Size(); ++i)
                        if (m_names[i] == current)
                            selected = static_cast<i32>(i);
                m_combo->SetSelectedIndex(selected);
                m_combo->SetEnabled(!IsReadOnly());
            }
            else
            {
                m_field->SetPlaceholder(value.Mixed ? foundation::gui::MixedValueText() : StringView{});
                m_field->SetText(current.AsView());
                m_field->SetReadOnly(IsReadOnly());
            }
            m_binding = false;
        }
        void OnLayout() override
        {
            m_combo->Arrange(GetContentBounds());
            m_field->Arrange(GetContentBounds());
        }
        void OnTextStyleChange() override
        {
            ShareTextStyle(*m_combo);
            ShareTextStyle(*m_field);
        }

    private:
        // The animator at or above the entity the inspector shows (its primary, for several).
        [[nodiscard]] engine::animation::IkAuthoringAnimator Animator() const
        {
            engine::animation::IkAuthoringAnimator animator;
            if (m_edit == nullptr)
                return animator;
            const Guid* primary = m_edit->EntitySelection().Primary();
            if (primary == nullptr)
                return animator;
            foundation::scene::Scene& scene = m_edit->Scene();
            const foundation::scene::EntityHandle entity = scene.FindEntity(*primary);
            if (!entity.IsAssigned() || !engine::animation::FindIkAuthoringAnimator(scene, entity, animator))
                return engine::animation::IkAuthoringAnimator{};
            return animator;
        }

        SceneEditContext* m_edit = nullptr;
        RefPtr<foundation::gui::ComboBox> m_combo;
        RefPtr<foundation::gui::TextField> m_field;
        Array<String> m_names; // the combo's items' names, "" for none
        bool m_listing = false;
        bool m_binding = false; // showing a value, not the user picking one
    };

    RTTI_DEFINE_OBJECT(BoneNameEditor, "rtti::editor::editor")
}
