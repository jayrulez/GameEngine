// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :property_editors partition
//
// The property grid's T1 editors (Specs/gui-lab-evolution-3.md), each the core's control as a row's
// value, mixed when the objects differ:
//   - bool: a check box, mixed its indeterminate state;
//   - integer and float: a number field, scrubbed (the scrub previewed, committed on release,
//     cancelled by Escape), its range and step the property's (unbounded when it has no range; a
//     float's step 0.01 when it has none), and the wheel left to the grid;
//   - string: a one-line field, committed on Return or on leaving it; Escape puts back what it had;
//   - enum: a combo box of its choices;
//   - flags: a button naming the flags set, opening a list of check boxes, a flag that differs
//     between the objects indeterminate; each one set or cleared alone.
// Mixed, a field shows the placeholder "Mixed" and a button says so.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui.toolkit:property_editors;

import foundation.core;
import experimental.gui;
import :property_source;
import :property_editor;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    /// What a field or a button shows for values that differ.
    [[nodiscard]] inline core::StringView MixedValueText() noexcept { return core::StringView(u8"Mixed"); }

    // An editor of one control filling it.
    class SingleControlEditor : public PropertyEditor
    {
        RTTI_OBJECT(SingleControlEditor, PropertyEditor)
    protected:
        void Hold(Node& control)
        {
            m_control = &control;
            AddChild(&control);
        }
        void OnLayout() override
        {
            if (m_control != nullptr)
                m_control->Arrange(GetContentBounds());
        }
        void OnTextStyleChange() override
        {
            if (UINode* control = core::Cast<UINode>(m_control))
                ShareTextStyle(*control);
        }

    private:
        Node* m_control = nullptr; // a child it holds
    };

    class BoolPropertyEditor : public SingleControlEditor
    {
        RTTI_OBJECT(BoolPropertyEditor, SingleControlEditor)
    public:
        BoolPropertyEditor()
        {
            AddClass(core::StringView(u8"bool"));
            m_box = core::MakeRef<CheckBox>(MemoryAllocator());
            BoolPropertyEditor* self = this;
            m_box->SetOnToggled([self](bool checked) { self->ApplyEdit(PropertyWrite{Variant(checked)}); });
            Hold(*m_box);
        }
        [[nodiscard]] CheckBox* GetCheckBox() const noexcept { return m_box.Get(); }

    protected:
        void OnBind(const PropertyInfo&, const PropertyValue& value) override
        {
            m_box->SetChecked(value.Value.AsBool());
            m_box->SetIndeterminate(value.Mixed);
            m_box->SetEnabled(!IsReadOnly());
        }

    private:
        core::RefPtr<CheckBox> m_box;
    };

    class NumberPropertyEditor : public SingleControlEditor
    {
        RTTI_OBJECT(NumberPropertyEditor, SingleControlEditor)
    public:
        NumberPropertyEditor()
        {
            AddClass(core::StringView(u8"number"));
            m_field = core::MakeRef<NumberField>(MemoryAllocator());
            m_field->SetStepsOnWheel(false); // the wheel scrolls the grid
            m_field->SetPlaceholder(MixedValueText());
            NumberPropertyEditor* self = this;
            m_field->SetOnValueChanging([self](f64 value)
            {
                self->Begin();
                self->PreviewEdit(PropertyWrite{self->ValueOf(value)});
            });
            m_field->SetOnValueChanged([self](f64 value)
            {
                self->Begin();
                self->m_begun = false;
                self->CommitEdit(PropertyWrite{self->ValueOf(value)});
            });
            m_field->SetOnValueCancelled([self]()
            {
                if (!self->m_begun)
                    return;
                self->m_begun = false;
                self->CancelEdit();
            });
            Hold(*m_field);
        }
        [[nodiscard]] NumberField* GetField() const noexcept { return m_field.Get(); }

    protected:
        void OnBind(const PropertyInfo& info, const PropertyValue& value) override
        {
            m_begun = false;
            m_integer = info.Kind == PropertyKind::Integer;
            m_field->SetInteger(m_integer);
            // Unbounded, it snaps to its step from zero and goes past the field's own range.
            m_field->SetMin(info.HasRange ? info.Min : 0.0);
            m_field->SetMax(info.HasRange ? info.Max : 100.0);
            m_field->SetAllowLesser(!info.HasRange);
            m_field->SetAllowGreater(!info.HasRange);
            m_field->SetStep(info.Step > 0.0 ? info.Step : (m_integer ? 1.0 : 0.01));
            m_field->SetValue(value.Value.AsFloat());
            m_field->SetMixed(value.Mixed);
            m_field->SetEnabled(!IsReadOnly());
        }

    private:
        [[nodiscard]] Variant ValueOf(f64 value) const
        {
            return m_integer ? Variant(static_cast<i64>(value < 0.0 ? value - 0.5 : value + 0.5)) : Variant(value);
        }
        void Begin()
        {
            if (m_begun)
                return;
            m_begun = true;
            BeginEdit();
        }

        core::RefPtr<NumberField> m_field;
        bool m_integer = false;
        bool m_begun = false; // a gesture under way (a scrub's previews)
    };

    class StringPropertyEditor : public SingleControlEditor
    {
        RTTI_OBJECT(StringPropertyEditor, SingleControlEditor)
    public:
        StringPropertyEditor()
        {
            AddClass(core::StringView(u8"string"));
            m_field = core::MakeRef<TextField>(MemoryAllocator());
            m_field->SetPlaceholder(MixedValueText());
            StringPropertyEditor* self = this;
            m_field->SetOnTextChanged([self](core::StringView text) { self->ApplyEdit(PropertyWrite{Variant(text)}); });
            Hold(*m_field);
        }
        [[nodiscard]] TextField* GetField() const noexcept { return m_field.Get(); }

    protected:
        void OnBind(const PropertyInfo& info, const PropertyValue& value) override
        {
            (void)info;
            m_shown = value.Mixed ? core::String{} : core::String(value.Value.AsString());
            m_field->SetPlaceholder(value.Mixed ? MixedValueText() : core::StringView{});
            m_field->SetText(m_shown.AsView());
            m_field->SetReadOnly(IsReadOnly());
        }
        // Escape puts back what it had (the field then has nothing new to commit).
        void OnKeyDown(const KeyEvent& event) override
        {
            if (static_cast<KeyCode>(event.KeyCode) == KeyCode::Escape && event.Modifiers == 0 && m_field->GetText() != m_shown.AsView())
            {
                m_field->SetText(m_shown.AsView());
                event.Handled = true;
            }
        }

    private:
        core::RefPtr<TextField> m_field;
        core::String m_shown;
    };

    class EnumPropertyEditor : public SingleControlEditor
    {
        RTTI_OBJECT(EnumPropertyEditor, SingleControlEditor)
    public:
        EnumPropertyEditor()
        {
            AddClass(core::StringView(u8"enum"));
            m_combo = core::MakeRef<ComboBox>(MemoryAllocator());
            m_combo->SetPlaceholder(core::String(MixedValueText()));
            EnumPropertyEditor* self = this;
            m_combo->SetOnSelectionChanged([self](i32 index)
            {
                if (self->m_binding || index < 0 || static_cast<usize>(index) >= self->m_choices.Size())
                    return;
                self->ApplyEdit(PropertyWrite{Variant(self->m_choices[static_cast<usize>(index)].Value)});
            });
            Hold(*m_combo);
        }
        [[nodiscard]] ComboBox* GetComboBox() const noexcept { return m_combo.Get(); }

    protected:
        void OnBind(const PropertyInfo& info, const PropertyValue& value) override
        {
            m_binding = true;
            if (!SameChoices(info.Choices))
            {
                m_choices = info.Choices;
                m_combo->SetItemsText(core::String{});
                for (const PropertyChoice& choice : m_choices)
                    m_combo->AddItem(choice.Name.AsView());
            }
            i32 selected = -1;
            if (!value.Mixed)
                for (usize i = 0; i < m_choices.Size(); ++i)
                    if (m_choices[i].Value == value.Value.AsInt())
                        selected = static_cast<i32>(i);
            m_combo->SetSelectedIndex(selected);
            m_combo->SetEnabled(!IsReadOnly());
            m_binding = false;
        }

    private:
        [[nodiscard]] bool SameChoices(const core::Array<PropertyChoice>& choices) const
        {
            if (choices.Size() != m_choices.Size())
                return false;
            for (usize i = 0; i < choices.Size(); ++i)
                if (choices[i].Value != m_choices[i].Value || choices[i].Name != m_choices[i].Name)
                    return false;
            return true;
        }

        core::RefPtr<ComboBox> m_combo;
        core::Array<PropertyChoice> m_choices;
        bool m_binding = false; // showing a value, not the user picking one
    };

    /// The list a flags editor opens: a check box per flag.
    class PropertyFlagsList : public UIWidget
    {
        RTTI_OBJECT(PropertyFlagsList, UIWidget)
    public:
        PropertyFlagsList()
        {
            SetTag(core::StringView(u8"propertyflagslist"));
            SetPadding(Thickness{6.0f, 4.0f, 6.0f, 4.0f});
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Column;
                s.RowGap = 2.0f;
            });
        }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            if (!BoxFills())
                ctx.VG().FillRect(localBounds.ToRectangle(), Color{0.18f, 0.20f, 0.25f, 1.0f});
            UIWidget::OnDraw(ctx, localBounds);
        }
    };

    class FlagsPropertyEditor : public SingleControlEditor
    {
        RTTI_OBJECT(FlagsPropertyEditor, SingleControlEditor)
    public:
        FlagsPropertyEditor()
        {
            AddClass(core::StringView(u8"flags"));
            m_button = core::MakeRef<Button>(MemoryAllocator()); // the sheet's `propertyeditor.flags button` draws it as a field
            FlagsPropertyEditor* self = this;
            m_button->SetOnClick([self]() { self->OpenList(); });
            Hold(*m_button);
            m_list = core::MakeRef<PropertyFlagsList>(MemoryAllocator());
        }
        [[nodiscard]] Button* GetButton() const noexcept { return m_button.Get(); }
        /// The list, while it is open.
        [[nodiscard]] PropertyFlagsList* GetList() const
        {
            const EventDispatcher* dispatcher = m_list->GetEventDispatcher();
            return dispatcher != nullptr && dispatcher->IsPopupOpen(*m_list) ? m_list.Get() : nullptr;
        }
        /// The list's check box for choice `index`, while it is open.
        [[nodiscard]] CheckBox* GetFlagBox(usize index) const { return GetList() != nullptr && index < m_boxes.Size() ? m_boxes[index].Get() : nullptr; }
        void OpenList()
        {
            EventDispatcher* dispatcher = GetEventDispatcher();
            if (dispatcher == nullptr || m_choices.IsEmpty() || IsReadOnly())
                return;
            ShareTextStyle(*m_list, false);
            ShowBoxes();
            PopupOptions options;
            options.Anchor = m_button.Get();
            options.Placement = PopupPlacement{PopupSide::Below, PopupAlign::Start, 0.0f, true};
            options.TakesFocus = true;
            options.FitContent = true;
            dispatcher->OpenPopup(*this, *m_list, core::Move(options));
        }

    protected:
        void OnBind(const PropertyInfo& info, const PropertyValue& value) override
        {
            m_choices = info.Choices;
            m_bits = static_cast<u64>(value.Value.AsInt());
            m_mixedBits = value.MixedBits;
            m_button->SetText(Summary().AsView());
            m_button->SetEnabled(!IsReadOnly());
            if (GetList() != nullptr)
                ShowBoxes();
        }

    private:
        // The flags set (those that differ said to), "None" when none is.
        [[nodiscard]] core::String Summary() const
        {
            if (m_mixedBits != 0)
                return core::String(MixedValueText());
            core::String text;
            for (const PropertyChoice& choice : m_choices)
                if (choice.Value != 0 && (m_bits & static_cast<u64>(choice.Value)) == static_cast<u64>(choice.Value))
                {
                    if (text.Size() != 0)
                        text.Append(core::StringView(u8", "));
                    text.Append(choice.Name.AsView());
                }
            return text.Size() != 0 ? text : core::String(u8"None");
        }
        // A check box per flag, made as the choices need them, each showing its bit.
        void ShowBoxes()
        {
            while (m_boxes.Size() < m_choices.Size())
            {
                auto box = core::MakeRef<CheckBox>(MemoryAllocator());
                const usize index = m_boxes.Size();
                FlagsPropertyEditor* self = this;
                box->SetOnToggled([self, index](bool checked) { self->SetFlag(index, checked); });
                m_list->AddChild(box.Get());
                m_boxes.PushBack(core::Move(box));
            }
            for (usize i = 0; i < m_boxes.Size(); ++i)
            {
                CheckBox& box = *m_boxes[i];
                box.SetDisplayed(i < m_choices.Size());
                if (i >= m_choices.Size())
                    continue;
                const u64 bit = static_cast<u64>(m_choices[i].Value);
                box.SetText(m_choices[i].Name.AsView());
                box.SetChecked((m_bits & bit) == bit && bit != 0);
                box.SetIndeterminate((m_mixedBits & bit) != 0);
            }
        }
        void SetFlag(usize index, bool set)
        {
            if (index >= m_choices.Size())
                return;
            const u64 bit = static_cast<u64>(m_choices[index].Value);
            ApplyEdit(PropertyWrite{Variant(static_cast<i64>(set ? bit : 0)), bit});
        }

        core::RefPtr<Button> m_button;
        core::RefPtr<PropertyFlagsList> m_list;
        core::Array<core::RefPtr<CheckBox>> m_boxes;
        core::Array<PropertyChoice> m_choices;
        u64 m_bits = 0;
        u64 m_mixedBits = 0;
    };

    /// Registers the T1 editors with `registry`: bool, integer, float, string, enum and flags.
    inline void RegisterDefaultPropertyEditors(PropertyEditorRegistry& registry)
    {
        registry.RegisterKind(PropertyKind::Bool, [](core::IAllocator& a) -> core::RefPtr<PropertyEditor> { return core::MakeRef<BoolPropertyEditor>(a); });
        registry.RegisterKind(PropertyKind::Integer, [](core::IAllocator& a) -> core::RefPtr<PropertyEditor> { return core::MakeRef<NumberPropertyEditor>(a); });
        registry.RegisterKind(PropertyKind::Float, [](core::IAllocator& a) -> core::RefPtr<PropertyEditor> { return core::MakeRef<NumberPropertyEditor>(a); });
        registry.RegisterKind(PropertyKind::String, [](core::IAllocator& a) -> core::RefPtr<PropertyEditor> { return core::MakeRef<StringPropertyEditor>(a); });
        registry.RegisterKind(PropertyKind::Enum, [](core::IAllocator& a) -> core::RefPtr<PropertyEditor> { return core::MakeRef<EnumPropertyEditor>(a); });
        registry.RegisterKind(PropertyKind::Flags, [](core::IAllocator& a) -> core::RefPtr<PropertyEditor> { return core::MakeRef<FlagsPropertyEditor>(a); });
    }
}
