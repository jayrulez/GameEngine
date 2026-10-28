// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :tool_panel_widgets partition
//
// What every viewport tool's panel is made of (Documentation/Specs/editor-gui.md, 5.3, layer 5,
// part 3), so every panel reads the same (the catalog's 17: three mode-picker styles, two ways to
// build a check row): a segmented row of exclusive choices (a mode, a layer, by word or by
// picture), a named slider with its number, a named check and a line of help. The body asks each
// control's value of the tool again when Sync runs (the host runs it each frame), since a tool's
// keys change it too (1 to 4 its mode, Shift + wheel its radius).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:tool_panel_widgets;

import foundation.core;
import foundation.gui;

using namespace foundation::core;
namespace core = foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class ToolPanelBody final : public gui::UIWidget
    {
        RTTI_OBJECT(ToolPanelBody, gui::UIWidget)
    public:
        /// One choice of a segmented row: its word, or its picture (asked again until it has one,
        /// as a thumbnail arrives later) with `icon` meanwhile; what it does, its tooltip.
        struct Choice
        {
            String label;
            String tooltip;
            String icon;                                // a picture's stand-in (an asset type's icon)
            Function<RefPtr<gui::Drawable>()> picture;  // empty: its word
        };

        ToolPanelBody() { AddClass(u8"tool-panel-body"); }

        /// A row of exclusive choices under `name` (none: no heading): `get` the chosen one's
        /// index, `set` choosing.
        void AddChoices(StringView name, Array<Choice> choices, Function<i32()> get, Function<void(i32)> set)
        {
            if (!name.IsEmpty())
                AddHeading(name);
            auto row = MakeRef<gui::Panel>(MemoryAllocator());
            row->AddClass(u8"segmented");
            ChoiceRow group;
            group.get = core::Move(get);
            ToolPanelBody* self = this;
            const usize index = m_choices.Size();
            for (usize i = 0; i < choices.Size(); ++i)
            {
                Choice& choice = choices[i];
                auto button = MakeRef<gui::Button>(MemoryAllocator());
                button->AddClass(u8"segment");
                button->SetToggle(true);
                button->SetTooltip(choice.tooltip.AsView());
                ChoicePicture picture;
                if (choice.picture)
                {
                    button->AddClass(u8"picture");
                    auto image = MakeRef<gui::Image>(MemoryAllocator());
                    image->AddClass(u8"segment-picture");
                    image->SetSource(choice.icon);
                    image->SetHitTestVisible(false);
                    button->AddChild(image.Get());
                    picture.image = image.Get();
                    picture.ask = core::Move(choice.picture);
                }
                else
                    button->SetText(choice.label.AsView());
                button->SetOnClick([self, index, i]()
                                   {
                                       ChoiceRow& owner = self->m_choices[index];
                                       if (owner.set)
                                           owner.set(static_cast<i32>(i));
                                       self->Sync();
                                   });
                group.buttons.PushBack(button.Get());
                group.pictures.PushBack(core::Move(picture));
                row->AddChild(button.Get());
            }
            group.set = core::Move(set);
            m_choices.PushBack(core::Move(group));
            AddChild(row.Get());
        }

        /// A picture shown, not chosen, under `name` (a terrain's base layer, which erasing
        /// reveals): `shown`'s picture once it has one, its icon meanwhile, its tooltip.
        void AddPicture(StringView name, Choice shown)
        {
            if (!name.IsEmpty())
                AddHeading(name);
            auto row = MakeRef<gui::Panel>(MemoryAllocator());
            row->AddClass(u8"segmented");
            auto frame = MakeRef<gui::Panel>(MemoryAllocator());
            frame->AddClass(u8"segment");
            frame->AddClass(u8"picture");
            frame->AddClass(u8"shown"); // not a button: nothing to choose
            frame->SetTooltip(shown.tooltip.AsView());
            auto image = MakeRef<gui::Image>(MemoryAllocator());
            image->AddClass(u8"segment-picture");
            image->SetSource(shown.icon);
            image->SetHitTestVisible(false);
            frame->AddChild(image.Get());
            row->AddChild(frame.Get());
            m_pictures.PushBack(ChoicePicture{image.Get(), core::Move(shown.picture), false});
            AddChild(row.Get());
        }

        /// A slider named `name` with its number beside it, between `min` and `max` by `step`.
        void AddSlider(StringView name, f64 min, f64 max, f64 step, Function<f64()> get, Function<void(f64)> set)
        {
            auto row = MakeRef<gui::Panel>(MemoryAllocator());
            row->AddClass(u8"hud-row");
            auto label = MakeRef<gui::Label>(MemoryAllocator());
            label->AddClass(u8"hud-name");
            label->SetText(name);
            row->AddChild(label.Get());
            auto field = MakeRef<gui::Panel>(MemoryAllocator());
            field->AddClass(u8"slider-field");
            auto slider = MakeRef<gui::Slider>(MemoryAllocator());
            slider->SetMin(min);
            slider->SetMax(max);
            slider->SetStep(step);
            auto number = MakeRef<gui::NumberField>(MemoryAllocator());
            number->SetMin(min);
            number->SetMax(max);
            number->SetStep(step);
            SliderRow slide;
            slide.slider = slider.Get();
            slide.number = number.Get();
            slide.get = core::Move(get);
            slide.set = core::Move(set);
            const usize index = m_slides.Size();
            ToolPanelBody* self = this;
            const auto take = [self, index](f64 value)
            {
                SliderRow& owner = self->m_slides[index];
                if (owner.set)
                    owner.set(value);
                self->Sync();
            };
            slider->SetOnValueChanging(take);
            slider->SetOnValueChanged(take);
            number->SetOnValueChanged(take);
            field->AddChild(slider.Get());
            field->AddChild(number.Get());
            row->AddChild(field.Get());
            m_slides.PushBack(core::Move(slide));
            AddChild(row.Get());
        }

        /// A check named `name`.
        void AddCheck(StringView name, Function<bool()> get, Function<void(bool)> set)
        {
            auto check = MakeRef<gui::CheckBox>(MemoryAllocator());
            check->SetText(name);
            const usize index = m_checks.Size();
            ToolPanelBody* self = this;
            check->SetOnToggled([self, index](bool on)
                                {
                                    CheckRow& owner = self->m_checks[index];
                                    if (owner.set)
                                        owner.set(on);
                                    self->Sync();
                                });
            m_checks.PushBack(CheckRow{check.Get(), core::Move(get), core::Move(set)});
            AddChild(check.Get());
        }

        /// A line of help (how the keys work it), or a note `text` asks again (hidden while it
        /// says nothing).
        void AddHelp(StringView text) { AddNote([help = String(text)]() { return help; }); }
        void AddNote(Function<String()> text)
        {
            auto label = MakeRef<gui::Label>(MemoryAllocator());
            label->AddClass(u8"hud-help");
            m_notes.PushBack(NoteRow{label.Get(), core::Move(text)});
            AddChild(label.Get());
        }

        /// Each control as the tool has it now.
        void Sync()
        {
            for (ChoiceRow& group : m_choices)
            {
                const i32 chosen = group.get ? group.get() : -1;
                for (usize i = 0; i < group.buttons.Size(); ++i)
                {
                    const bool on = static_cast<i32>(i) == chosen;
                    if (group.buttons[i]->IsChecked() != on)
                        group.buttons[i]->SetChecked(on);
                    ChoicePicture& picture = group.pictures[i];
                    if (picture.ask && !picture.shown)
                        if (RefPtr<gui::Drawable> drawable = picture.ask())
                        {
                            picture.image->SetDrawable(drawable);
                            picture.shown = true;
                        }
                }
            }
            for (ChoicePicture& picture : m_pictures)
                if (picture.ask && !picture.shown)
                    if (RefPtr<gui::Drawable> drawable = picture.ask())
                    {
                        picture.image->SetDrawable(drawable);
                        picture.shown = true;
                    }
            for (SliderRow& slide : m_slides)
            {
                if (!slide.get)
                    continue;
                const f64 value = slide.get();
                if (slide.slider->GetValue() != value)
                    slide.slider->SetValue(value);
                if (!slide.number->IsEditing() && slide.number->GetValue() != value)
                    slide.number->SetValue(value);
            }
            for (CheckRow& check : m_checks)
                if (check.get && check.box->IsChecked() != check.get())
                    check.box->SetChecked(check.get());
            for (NoteRow& note : m_notes)
            {
                const String text = note.text ? note.text() : String{};
                if (note.label->GetText() != text.AsView())
                    note.label->SetText(text.AsView());
                note.label->SetDisplayed(!text.IsEmpty());
            }
        }

        // === For tests ===
        [[nodiscard]] gui::Button* ChoiceAt(usize group, usize index) const
        {
            return group < m_choices.Size() && index < m_choices[group].buttons.Size() ? m_choices[group].buttons[index] : nullptr;
        }
        [[nodiscard]] usize ChoiceCount(usize group) const { return group < m_choices.Size() ? m_choices[group].buttons.Size() : 0u; }
        [[nodiscard]] gui::NumberField* NumberAt(usize index) const { return index < m_slides.Size() ? m_slides[index].number : nullptr; }
        [[nodiscard]] gui::CheckBox* CheckAt(usize index) const { return index < m_checks.Size() ? m_checks[index].box : nullptr; }
        /// The `index`th shown picture's image (AddPicture's); null: none.
        [[nodiscard]] gui::Image* PictureAt(usize index) const { return index < m_pictures.Size() ? m_pictures[index].image : nullptr; }

    private:
        void AddHeading(StringView name)
        {
            auto heading = MakeRef<gui::Label>(MemoryAllocator());
            heading->AddClass(u8"hud-heading");
            heading->SetText(name);
            AddChild(heading.Get());
        }
        struct ChoicePicture
        {
            gui::Image* image = nullptr;
            Function<RefPtr<gui::Drawable>()> ask;
            bool shown = false;
        };
        struct ChoiceRow
        {
            Array<gui::Button*> buttons; // its children's, which it keeps
            Array<ChoicePicture> pictures;
            Function<i32()> get;
            Function<void(i32)> set;
        };
        struct SliderRow
        {
            gui::Slider* slider = nullptr;
            gui::NumberField* number = nullptr;
            Function<f64()> get;
            Function<void(f64)> set;
        };
        struct CheckRow
        {
            gui::CheckBox* box = nullptr;
            Function<bool()> get;
            Function<void(bool)> set;
        };
        struct NoteRow
        {
            gui::Label* label = nullptr;
            Function<String()> text;
        };
        Array<ChoiceRow> m_choices;
        Array<ChoicePicture> m_pictures; // AddPicture's: shown, not chosen
        Array<SliderRow> m_slides;
        Array<CheckRow> m_checks;
        Array<NoteRow> m_notes;
    };

    RTTI_DEFINE_OBJECT(ToolPanelBody, "rtti::editor::editor::app")
}
