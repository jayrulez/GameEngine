// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :property_editors partition
//
// The property grid's default editors. Each uses one of the core GUI's controls as a row's value,
// and shows a mixed state when the objects' values differ:
//   - bool: a check box, which shows mixed as its indeterminate state.
//   - integer and float: a number field that can be scrubbed. The scrub previews, commits on
//     release and cancels on Escape. Its range and step are the property's; with no range it is
//     unbounded, and a float with no step uses 0.01. The wheel is left to the grid.
//   - a number on a slider (hint "slider"): a slider over its range beside its number field. Each
//     works the same way: a drag previews, commits on release, and Escape puts the value back. The
//     wheel is left to the grid.
//   - string: a one-line field that commits on Return or when focus leaves it. Escape restores
//     what it had.
//   - enum: a combo box of its choices.
//   - flags: a button naming the flags set, which opens a list of check boxes. A flag that differs
//     between the objects is indeterminate. Each flag is set or cleared on its own.
//   - Float2, Float3 and Float4: a group hinted with Core's type name. Its row holds a number field
//     per child, each labelled with the child's name and editing the child's path, so each one is
//     mixed and scrubbed on its own.
//   - colours: a swatch and a hex field. The swatch opens the core picker as a popup. A drag in it
//     previews on every object, and closing the picker commits once (after putting the originals
//     back); Escape cancels. The hex field commits what is typed.
//   - an intensity in exposure stops (hint "stops"): a number field showing log2 of the value, so
//     0 stops is x1; it writes 2^stops.
//   - an HDR colour (hint "ColorHdr"): the editor for its `color` part and the stops for its
//     `intensity` part on one row, each with its own gesture.
//   - lists: the count (mixed when the objects' counts differ) and an Add button, which offers a
//     menu of types when the list takes several. Add appends the element at the end.
// When mixed, a field shows the placeholder "Mixed" and a button shows that text.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui.toolkit:property_editors;

import foundation.core;
import foundation.gui;
import :property_source;
import :property_editor;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// Returns what a field or a button shows for values that differ.
    [[nodiscard]] inline core::StringView MixedValueText() noexcept { return core::StringView(u8"Mixed"); }

    // An editor made of one control that fills it.
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
        Node* m_control = nullptr; // the child control it holds
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
        [[nodiscard]] bool IsTyping() const override { return m_field->IsEditing(); }
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
            // With no range, the value snaps to its step from zero and may go past the field's
            // default range.
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
        bool m_begun = false; // set while a gesture (a scrub's previews) is under way
    };

    /// A number over its range: a slider beside its field. Each control edits in one gesture: it
    /// previews as it goes, commits on release (the field on Return or when focus leaves it), and
    /// Escape puts the value back.
    class SliderPropertyEditor : public PropertyEditor
    {
        RTTI_OBJECT(SliderPropertyEditor, PropertyEditor)
    public:
        [[nodiscard]] bool IsTyping() const override { return m_field->IsEditing(); }
        SliderPropertyEditor()
        {
            AddClass(core::StringView(u8"slider-field"));
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Row;
                s.AlignItems = Align::Center;
                s.ColumnGap = 8.0f;
            });
            SliderPropertyEditor* self = this;
            m_slider = core::MakeRef<Slider>(MemoryAllocator());
            m_slider->SetStepsOnWheel(false); // the wheel scrolls the grid
            m_slider->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Grow = 1.0f;
                s.MinWidth = LayoutLength::Px(0.0f);
            });
            m_slider->SetOnValueChanging([self](f64 value)
            {
                self->Begin();
                self->m_field->SetValue(value);
                self->PreviewEdit(PropertyWrite{self->ValueOf(value)});
            });
            m_slider->SetOnValueChanged([self](f64 value) { self->Commit(value); });
            m_slider->SetOnValueCancelled([self]()
            {
                self->m_field->SetValue(self->m_slider->GetValue()); // the field follows the slider back
                self->Cancel();
            });
            AddChild(m_slider.Get());
            m_field = core::MakeRef<NumberField>(MemoryAllocator());
            m_field->SetStepsOnWheel(false);
            m_field->SetPlaceholder(MixedValueText());
            m_field->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Width = LayoutLength::Px(64.0f);
                s.Shrink = 0.0f;
            });
            m_field->SetOnValueChanging([self](f64 value)
            {
                self->Begin();
                self->m_slider->SetValue(value);
                self->PreviewEdit(PropertyWrite{self->ValueOf(value)});
            });
            m_field->SetOnValueChanged([self](f64 value) { self->Commit(value); });
            m_field->SetOnValueCancelled([self]()
            {
                self->m_slider->SetValue(self->m_field->GetValue()); // the slider follows the field back
                self->Cancel();
            });
            AddChild(m_field.Get());
        }
        [[nodiscard]] Slider* GetSlider() const noexcept { return m_slider.Get(); }
        [[nodiscard]] NumberField* GetField() const noexcept { return m_field.Get(); }

    protected:
        void OnBind(const PropertyInfo& info, const PropertyValue& value) override
        {
            m_begun = false;
            m_integer = info.Kind == PropertyKind::Integer;
            // A slider needs both ends: without a range it runs 0 to 1.
            const f64 lo = info.HasRange ? info.Min : 0.0;
            const f64 hi = info.HasRange ? info.Max : 1.0;
            const f64 step = info.Step > 0.0 ? info.Step : (m_integer ? 1.0 : 0.01);
            m_slider->SetMin(lo);
            m_slider->SetMax(hi);
            m_slider->SetStep(step);
            m_field->SetInteger(m_integer);
            m_field->SetMin(lo);
            m_field->SetMax(hi);
            m_field->SetAllowLesser(false);
            m_field->SetAllowGreater(false);
            m_field->SetStep(step);
            m_slider->SetValue(value.Value.AsFloat());
            m_field->SetValue(value.Value.AsFloat());
            m_field->SetMixed(value.Mixed);
            m_slider->SetEnabled(!IsReadOnly());
            m_field->SetEnabled(!IsReadOnly());
        }
        void OnTextStyleChange() override { ShareTextStyle(*m_field); }

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
        void Commit(f64 value)
        {
            Begin();
            m_begun = false;
            m_slider->SetValue(value);
            m_field->SetValue(value);
            CommitEdit(PropertyWrite{ValueOf(value)});
        }
        void Cancel()
        {
            if (!m_begun)
                return;
            m_begun = false;
            CancelEdit();
        }

        core::RefPtr<Slider> m_slider;
        core::RefPtr<NumberField> m_field;
        bool m_integer = false;
        bool m_begun = false; // set while a gesture (a drag's or a scrub's previews) is under way
    };

    class StringPropertyEditor : public SingleControlEditor
    {
        RTTI_OBJECT(StringPropertyEditor, SingleControlEditor)
    public:
        [[nodiscard]] bool IsTyping() const override { return m_field->IsFocused(); }
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
        // Escape restores what the field had, so it then has nothing new to commit.
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
        bool m_binding = false; // set while showing a value, so the change is not the user's pick
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
        /// Returns the list while it is open, else null.
        [[nodiscard]] PropertyFlagsList* GetList() const
        {
            const EventDispatcher* dispatcher = m_list->GetEventDispatcher();
            return dispatcher != nullptr && dispatcher->IsPopupOpen(*m_list) ? m_list.Get() : nullptr;
        }
        /// Returns the list's check box for choice `index` while the list is open, else null.
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
        // Returns the names of the flags set, "None" when none is, or "Mixed" when any flag differs
        // between the objects.
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
        // Shows a check box per flag, making boxes as the choices need them, each showing its bit.
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

    /// Shows a group's number children side by side on its row (Float2, Float3, Float4). Each is a
    /// number editor for its own path, after a label with its name.
    class FloatsPropertyEditor : public PropertyEditor
    {
        RTTI_OBJECT(FloatsPropertyEditor, PropertyEditor)
    public:
        [[nodiscard]] bool IsTyping() const override
        {
            for (usize i = 0; i < m_shown && i < m_fields.Size(); ++i)
                if (m_fields[i]->IsTyping())
                    return true;
            return false;
        }
        FloatsPropertyEditor() : m_labels(MemoryAllocator()), m_fields(MemoryAllocator())
        {
            AddClass(core::StringView(u8"floats"));
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Row;
                s.AlignItems = Align::Stretch;
                s.ColumnGap = 6.0f;
            });
        }
        [[nodiscard]] usize GetPartCount() const noexcept { return m_shown; }
        [[nodiscard]] NumberPropertyEditor* GetPart(usize index) const { return index < m_shown ? m_fields[index].Get() : nullptr; }
        [[nodiscard]] Label* GetPartLabel(usize index) const { return index < m_shown ? m_labels[index].Get() : nullptr; }

    protected:
        void OnBind(const PropertyInfo&, const PropertyValue&) override
        {
            IPropertyEditHost* host = GetHost();
            m_shown = 0;
            core::Array<PropertyInfo> parts(MemoryAllocator());
            if (host != nullptr)
                host->GetChildProperties(GetPath(), parts);
            for (PropertyInfo& part : parts)
            {
                if (!part.Visible || (part.Kind != PropertyKind::Float && part.Kind != PropertyKind::Integer))
                    continue;
                if (m_shown == m_fields.Size())
                    AddPart();
                const core::String path = JoinPropertyPath(GetPath(), part.Name.AsView());
                part.ReadOnly = part.ReadOnly || IsReadOnly();
                m_labels[m_shown]->SetText(part.ShownName());
                m_fields[m_shown]->Bind(*host, path.AsView(), part, host->ReadPropertyValue(path.AsView(), part.Kind));
                ++m_shown;
            }
            for (usize i = 0; i < m_fields.Size(); ++i)
            {
                m_labels[i]->SetDisplayed(i < m_shown);
                m_fields[i]->SetDisplayed(i < m_shown);
            }
        }
        void OnTextStyleChange() override
        {
            for (usize i = 0; i < m_fields.Size(); ++i)
            {
                ShareTextStyle(*m_labels[i]);
                ShareTextStyle(*m_fields[i]);
            }
        }

    private:
        void AddPart()
        {
            auto label = core::MakeRef<Label>(MemoryAllocator());
            label->AddClass(core::StringView(u8"component"));
            label->SetTextAlignment(TextHAlign::Start, TextVAlign::Middle);
            label->SetHitTestVisible(false);
            label->EditLayoutStyle([](LayoutStyle& s) { s.Shrink = 0.0f; });
            auto field = core::MakeRef<NumberPropertyEditor>(MemoryAllocator());
            field->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Grow = 1.0f;
                s.Shrink = 1.0f;
                s.Basis = LayoutLength::Px(0.0f);
                s.MinWidth = LayoutLength::Px(0.0f);
            });
            ShareTextStyle(*label);
            ShareTextStyle(*field);
            AddChild(label.Get());
            AddChild(field.Get());
            m_labels.PushBack(core::Move(label));
            m_fields.PushBack(core::Move(field));
        }

        core::Array<core::RefPtr<Label>> m_labels;
        core::Array<core::RefPtr<NumberPropertyEditor>> m_fields;
        usize m_shown = 0;
    };

    /// The popup a colour editor opens: the core GUI's picker on a panel.
    class PropertyColorPopup : public UIWidget
    {
        RTTI_OBJECT(PropertyColorPopup, UIWidget)
    public:
        PropertyColorPopup()
        {
            SetTag(core::StringView(u8"propertycolorpopup"));
            SetPadding(Thickness{8.0f, 8.0f, 8.0f, 8.0f});
            EditLayoutStyle([](LayoutStyle& s) { s.Width = LayoutLength::Px(300.0f); });
            m_picker = core::MakeRef<ColorPicker>(MemoryAllocator());
            AddChild(m_picker.Get());
        }
        [[nodiscard]] ColorPicker& GetPicker() const noexcept { return *m_picker; }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            if (!BoxFills())
                ctx.VG().FillRect(localBounds.ToRectangle(), Color{0.18f, 0.20f, 0.25f, 1.0f});
            UIWidget::OnDraw(ctx, localBounds);
        }

    private:
        core::RefPtr<ColorPicker> m_picker;
    };

    class ColorPropertyEditor : public PropertyEditor
    {
        RTTI_OBJECT(ColorPropertyEditor, PropertyEditor)
    public:
        [[nodiscard]] bool IsTyping() const override { return m_hex->IsFocused(); }
        ColorPropertyEditor()
        {
            AddClass(core::StringView(u8"color"));
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Row;
                s.AlignItems = Align::Stretch;
                s.ColumnGap = 6.0f;
            });
            ColorPropertyEditor* self = this;
            m_swatch = core::MakeRef<ColorSwatch>(MemoryAllocator());
            m_swatch->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Width = LayoutLength::Px(40.0f);
                s.Shrink = 0.0f;
            });
            m_swatch->SetOnClick([self]() { self->OpenPicker(); });
            AddChild(m_swatch.Get());
            m_hex = core::MakeRef<TextField>(MemoryAllocator());
            m_hex->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Grow = 1.0f;
                s.Basis = LayoutLength::Px(0.0f);
                s.MinWidth = LayoutLength::Px(0.0f);
            });
            m_hex->SetOnTextChanged([self](core::StringView text) { self->TypedHex(text); });
            AddChild(m_hex.Get());
            m_popup = core::MakeRef<PropertyColorPopup>(MemoryAllocator());
            m_popup->GetPicker().SetOnColorChanging([self](Color color) { self->Picked(color); });
            m_popup->GetPicker().SetOnColorChanged([self](Color color) { self->Picked(color); });
        }
        [[nodiscard]] ColorSwatch* GetSwatch() const noexcept { return m_swatch.Get(); }
        [[nodiscard]] TextField* GetHexField() const noexcept { return m_hex.Get(); }
        /// Returns the picker while it is open, else null.
        [[nodiscard]] ColorPicker* GetPicker() const
        {
            const EventDispatcher* dispatcher = m_popup->GetEventDispatcher();
            return dispatcher != nullptr && dispatcher->IsPopupOpen(*m_popup) ? &m_popup->GetPicker() : nullptr;
        }
        /// Opens the picker on the colour shown. Its drags preview, and closing it commits.
        void OpenPicker()
        {
            EventDispatcher* dispatcher = GetEventDispatcher();
            if (dispatcher == nullptr || IsReadOnly() || GetPicker() != nullptr)
                return;
            ShareTextStyle(*m_popup, false);
            m_popup->GetPicker().SetShowsAlpha(m_showsAlpha);
            m_popup->GetPicker().SetColor(m_color);
            m_popup->GetPicker().SetOriginal(m_color); // shown beside the new colour; a click on it restores it
            m_picking = false;
            m_picked = m_color;
            PopupOptions options;
            options.Anchor = m_swatch.Get();
            options.Placement = PopupPlacement{PopupSide::Below, PopupAlign::Start, 2.0f, true};
            options.TakesFocus = true;
            options.FitContent = true;
            ColorPropertyEditor* self = this;
            options.OnClosed = [self](PopupCloseReason reason) { self->Closed(reason == PopupCloseReason::Escape); };
            dispatcher->OpenPopup(*this, *m_popup, core::Move(options));
        }

    protected:
        void OnBind(const PropertyInfo& info, const PropertyValue& value) override
        {
            m_showsAlpha = info.ShowsAlpha;
            m_color = value.Value.GetType() == Variant::Type::Color ? value.Value.AsColor() : Color{};
            if (!m_showsAlpha)
                m_color.a = 1.0f;
            m_swatch->SetColor(m_color);
            m_swatch->SetMixed(value.Mixed);
            m_swatch->SetShowsAlpha(m_showsAlpha);
            m_swatch->SetEnabled(!IsReadOnly());
            m_hex->SetPlaceholder(value.Mixed ? MixedValueText() : core::StringView{});
            m_hex->SetText(value.Mixed ? core::StringView{} : FormatHexColor(m_color, m_showsAlpha).AsView());
            m_hex->SetReadOnly(IsReadOnly());
        }
        void OnTextStyleChange() override
        {
            ShareTextStyle(*m_swatch);
            ShareTextStyle(*m_hex);
        }

    private:
        // Handles each step of the picker: the first step begins the gesture, and each is previewed.
        void Picked(Color color)
        {
            if (!m_showsAlpha)
                color.a = 1.0f;
            if (!m_picking)
            {
                m_picking = true;
                BeginEdit();
            }
            m_picked = color;
            m_swatch->SetColor(color);
            m_swatch->SetMixed(false);
            m_hex->SetPlaceholder(core::StringView{});
            m_hex->SetText(FormatHexColor(color, m_showsAlpha).AsView()); // the row shows the picked colour as it changes
            PreviewEdit(PropertyWrite{Variant(color)});
        }
        // Handles the picker closing: what it picked is committed once, or cancelled on Escape.
        void Closed(bool escaped)
        {
            if (!m_picking)
                return;
            m_picking = false;
            if (escaped)
                CancelEdit();
            else
                CommitEdit(PropertyWrite{Variant(m_picked)});
        }
        void TypedHex(core::StringView text)
        {
            const Optional<Color> parsed = ParseColor(core::Trim(text));
            if (!parsed.HasValue())
            {
                m_hex->SetText(FormatHexColor(m_color, m_showsAlpha).AsView()); // not a colour, so show the old value again
                return;
            }
            Color color = parsed.Value();
            if (!m_showsAlpha)
                color.a = 1.0f;
            ApplyEdit(PropertyWrite{Variant(color)});
        }

        core::RefPtr<ColorSwatch> m_swatch;
        core::RefPtr<TextField> m_hex;
        core::RefPtr<PropertyColorPopup> m_popup;
        Color m_color{};
        Color m_picked{};
        bool m_showsAlpha = true;
        bool m_picking = false; // set while a gesture is under way in the open picker
    };

    /// Shows an intensity in exposure stops, the log2 of its value (0 stops is x1), and writes
    /// 2^stops.
    class StopsPropertyEditor : public SingleControlEditor
    {
        RTTI_OBJECT(StopsPropertyEditor, SingleControlEditor)
    public:
        [[nodiscard]] bool IsTyping() const override { return m_field->IsEditing(); }
        StopsPropertyEditor()
        {
            AddClass(core::StringView(u8"stops"));
            m_field = core::MakeRef<NumberField>(MemoryAllocator());
            m_field->SetStepsOnWheel(false);
            m_field->SetPlaceholder(MixedValueText());
            m_field->SetSuffix(core::String(u8" EV"));
            m_field->SetMin(-16.0);
            m_field->SetMax(16.0);
            m_field->SetStep(0.1);
            StopsPropertyEditor* self = this;
            m_field->SetOnValueChanging([self](f64 stops)
            {
                self->Begin();
                self->PreviewEdit(PropertyWrite{Variant(static_cast<f64>(core::Pow(2.0f, static_cast<f32>(stops))))});
            });
            m_field->SetOnValueChanged([self](f64 stops)
            {
                self->Begin();
                self->m_begun = false;
                self->CommitEdit(PropertyWrite{Variant(static_cast<f64>(core::Pow(2.0f, static_cast<f32>(stops))))});
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
        void OnBind(const PropertyInfo&, const PropertyValue& value) override
        {
            m_begun = false;
            const f64 linear = value.Value.AsFloat();
            m_field->SetValue(linear > 0.0 ? static_cast<f64>(core::Log(static_cast<f32>(linear)) / core::Log(2.0f)) : -16.0);
            m_field->SetMixed(value.Mixed);
            m_field->SetEnabled(!IsReadOnly());
        }

    private:
        void Begin()
        {
            if (m_begun)
                return;
            m_begun = true;
            BeginEdit();
        }

        core::RefPtr<NumberField> m_field;
        bool m_begun = false;
    };

    /// An HDR colour on one row: the swatch and hex for its `color` part, and the stops for its
    /// `intensity` part.
    class HdrColorPropertyEditor : public PropertyEditor
    {
        RTTI_OBJECT(HdrColorPropertyEditor, PropertyEditor)
    public:
        [[nodiscard]] bool IsTyping() const override { return m_color->IsTyping() || m_stops->IsTyping(); }
        HdrColorPropertyEditor()
        {
            AddClass(core::StringView(u8"colorhdr"));
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Row;
                s.AlignItems = Align::Stretch;
                s.ColumnGap = 6.0f;
            });
            m_color = core::MakeRef<ColorPropertyEditor>(MemoryAllocator());
            m_color->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Grow = 3.0f;
                s.Basis = LayoutLength::Px(0.0f);
                s.MinWidth = LayoutLength::Px(0.0f);
            });
            AddChild(m_color.Get());
            m_stops = core::MakeRef<StopsPropertyEditor>(MemoryAllocator());
            m_stops->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Grow = 2.0f;
                s.Basis = LayoutLength::Px(0.0f);
                s.MinWidth = LayoutLength::Px(0.0f);
            });
            AddChild(m_stops.Get());
        }
        [[nodiscard]] ColorPropertyEditor* GetColorPart() const noexcept { return m_color.Get(); }
        [[nodiscard]] StopsPropertyEditor* GetIntensityPart() const noexcept { return m_stops.Get(); }

    protected:
        void OnBind(const PropertyInfo&, const PropertyValue&) override
        {
            IPropertyEditHost* host = GetHost();
            if (host == nullptr)
                return;
            core::Array<PropertyInfo> parts(MemoryAllocator());
            host->GetChildProperties(GetPath(), parts);
            for (PropertyInfo& part : parts)
            {
                const core::String path = JoinPropertyPath(GetPath(), part.Name.AsView());
                part.ReadOnly = part.ReadOnly || IsReadOnly();
                if (part.Kind == PropertyKind::Color)
                    m_color->Bind(*host, path.AsView(), part, host->ReadPropertyValue(path.AsView(), part.Kind));
                else if (part.Kind == PropertyKind::Float)
                    m_stops->Bind(*host, path.AsView(), part, host->ReadPropertyValue(path.AsView(), part.Kind));
            }
        }
        void OnTextStyleChange() override
        {
            ShareTextStyle(*m_color);
            ShareTextStyle(*m_stops);
        }

    private:
        core::RefPtr<ColorPropertyEditor> m_color;
        core::RefPtr<StopsPropertyEditor> m_stops;
    };

    /// A list's row: the element count and an Add button.
    class ListPropertyEditor : public PropertyEditor
    {
        RTTI_OBJECT(ListPropertyEditor, PropertyEditor)
    public:
        ListPropertyEditor() : m_types(MemoryAllocator())
        {
            AddClass(core::StringView(u8"list"));
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Row;
                s.AlignItems = Align::Center;
                s.ColumnGap = 8.0f;
            });
            m_count = core::MakeRef<Label>(MemoryAllocator());
            m_count->AddClass(core::StringView(u8"count"));
            m_count->SetHitTestVisible(false);
            m_count->EditLayoutStyle([](LayoutStyle& s) { s.Grow = 1.0f; });
            AddChild(m_count.Get());
            m_add = core::MakeRef<Button>(MemoryAllocator());
            m_add->AddClass(core::StringView(u8"add"));
            m_add->SetText(core::StringView(u8"Add"));
            ListPropertyEditor* self = this;
            m_add->SetOnClick([self]() { self->Add(); });
            AddChild(m_add.Get());
        }
        [[nodiscard]] Label* GetCountLabel() const noexcept { return m_count.Get(); }
        [[nodiscard]] Button* GetAddButton() const noexcept { return m_add.Get(); }
        /// Returns the menu of types to add while it is open, else null.
        [[nodiscard]] Menu* GetTypeMenu() const noexcept { return m_menu && m_menu->IsOpen() ? m_menu.Get() : nullptr; }

    protected:
        void OnBind(const PropertyInfo& info, const PropertyValue& value) override
        {
            m_size = static_cast<usize>(value.Value.AsInt() < 0 ? 0 : value.Value.AsInt());
            m_types = info.ElementTypes;
            if (value.Mixed)
                m_count->SetText(MixedValueText());
            else
                m_count->SetText((m_size == 1 ? core::String(u8"1 element") : core::Format(u8"{} elements", m_size)).AsView());
            const bool full = info.MaxCount != 0 && !value.Mixed && m_size >= info.MaxCount;
            m_add->SetDisplayed((info.ListOps & PropertyListAdd) != 0);
            m_add->SetEnabled(!IsReadOnly() && !full);
        }
        void OnTextStyleChange() override
        {
            ShareTextStyle(*m_count);
            ShareTextStyle(*m_add);
        }

    private:
        // Adds an element at the end. When the list takes several types, the type comes from a menu.
        void Add()
        {
            if (m_types.Size() <= 1)
            {
                Insert(m_types.IsEmpty() ? core::StringView{} : m_types[0].AsView());
                return;
            }
            m_menu = core::MakeRef<Menu>(MemoryAllocator());
            ShareTextStyle(*m_menu, false);
            m_menu->SetAutoWidth(true);
            ListPropertyEditor* self = this;
            for (const core::String& type : m_types)
                (void)m_menu->AddItem(type.AsView(), [self, type]() { self->Insert(type.AsView()); });
            const Rect below = m_add->GetScreenBounds();
            Node* top = GetRootNode();
            if (top != nullptr)
                m_menu->Open(*this, top->ConvertToNodeSpace(core::Float2{below.x, below.y + below.height}));
        }
        void Insert(core::StringView type)
        {
            ListEdit edit;
            edit.Kind = ListEdit::Op::Insert;
            edit.Index = m_size;
            edit.ElementType = core::String(type);
            (void)EditList(edit);
        }

        core::RefPtr<Label> m_count;
        core::RefPtr<Button> m_add;
        core::RefPtr<Menu> m_menu;
        core::Array<core::String> m_types;
        usize m_size = 0;
    };
}
