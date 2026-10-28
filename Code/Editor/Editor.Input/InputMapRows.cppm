// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Input - :input_map_rows partition.
//
// What the input map page's grid says of a set's rows beyond what reflection says
// (Documentation/Specs/editor-gui.md, 5.4, layer 5, part 4): an action by its name and, closed, its
// kind and its bindings in a line; a binding by its source and, closed, its input; a binding's
// settings shown only for the sources that have them (a dead zone for what is analog, the screen
// area for touch, the four keys for a four-key binding), as the old page laid them out; the kinds
// and the sources by the page's own words; an input's row the capture's. The page's describe hook,
// a pure function of the set so its tests need no page.

module;
#include "Core/Prelude.h"

export module editor.input:input_map_rows;

import foundation.core;
import foundation.input;
import foundation.gui.toolkit;
import :input_map_edits;

using namespace foundation::core;

export namespace editor
{
    namespace gui = foundation::gui;

    /// The hint of a row whose value is an input a binding listens for (a key, a button, a stick):
    /// the page's capture editor takes it.
    inline constexpr StringView kInputCodeHint = u8"inputCode";

    namespace input_map_rows
    {
        // The leading index of `path` after `prefix` ("actions.2.bindings" after "actions.": 2), and
        // what follows it in `rest` ("bindings"); -1 when it does not begin so.
        [[nodiscard]] inline i64 IndexAfter(StringView path, StringView prefix, StringView& rest)
        {
            if (!path.StartsWith(prefix))
                return -1;
            const StringView tail = path.SubStr(prefix.Size(), path.Size() - prefix.Size());
            usize digits = 0;
            while (digits < tail.Size() && tail[digits] >= u8'0' && tail[digits] <= u8'9')
                ++digits;
            if (digits == 0)
                return -1;
            const Optional<i64> index = ParseInt(tail.SubStr(0, digits));
            rest = digits < tail.Size() && tail[digits] == u8'.' ? tail.SubStr(digits + 1, tail.Size() - digits - 1) : StringView{};
            return index.HasValue() ? index.Value() : -1;
        }

        [[nodiscard]] inline String BindingsLine(const input::Action& action)
        {
            if (action.bindings.IsEmpty())
                return String(u8"no binding yet");
            String line;
            for (const input::Binding& binding : action.bindings)
            {
                if (!line.IsEmpty())
                    line += u8", ";
                line += input::DescribeBinding(binding);
            }
            return line;
        }

        // A binding's setting: whether its source has it, and how it is named and bounded.
        inline void DescribeBindingField(const input::Binding& binding, StringView field, gui::PropertyInfo& info)
        {
            using Source = input::BindingSource;
            const Source source = binding.source;
            const bool composite = source == Source::Composite2D;
            const bool deadZone = source == Source::MouseAxis || source == Source::GamepadAxis || source == Source::GamepadStick || source == Source::TouchStick;
            const bool invert = source == Source::MouseAxis || source == Source::MouseDelta || source == Source::GamepadAxis || source == Source::GamepadStick ||
                                source == Source::TouchStick || composite;
            const bool gamepad = source == Source::GamepadButton || source == Source::GamepadAxis || source == Source::GamepadStick;
            const bool touch = source == Source::TouchButton || source == Source::TouchStick;
            const auto range = [&info](f64 min, f64 max, f64 step)
            {
                info.HasRange = true;
                info.Min = min;
                info.Max = max;
                info.Step = step;
            };
            if (field == u8"source")
            {
                info.DisplayName = String(u8"Source");
                for (gui::PropertyChoice& choice : info.Choices)
                    choice.Name = String(input_map_edit::SourceTitle(static_cast<Source>(choice.Value)));
            }
            else if (field == u8"code")
            {
                info.DisplayName = String(u8"Input");
                info.Hint = String(kInputCodeHint);
                info.Visible = !composite;
            }
            else if (field == u8"negX" || field == u8"posX" || field == u8"negY" || field == u8"posY")
            {
                info.DisplayName = String(field == u8"negX" ? StringView(u8"Left") : field == u8"posX" ? StringView(u8"Right") : field == u8"negY" ? StringView(u8"Down") : StringView(u8"Up"));
                info.Hint = String(kInputCodeHint);
                info.Visible = composite;
            }
            else if (field == u8"modifiers")
                info.Visible = false; // a chord's modifiers: not authored on this page
            else if (field == u8"device")
            {
                // A choice, as the old page had it: Any, or a pad by its number from 1 (the device
                // its index from 0, -1 for any).
                info.DisplayName = String(u8"Gamepad");
                info.Description = String(u8"Which connected gamepad: any, or one by its number");
                info.Kind = gui::PropertyKind::Enum;
                info.Choices.Clear();
                info.Choices.PushBack(gui::PropertyChoice{String(u8"Any"), -1});
                for (i64 pad = 0; pad < 4; ++pad)
                    info.Choices.PushBack(gui::PropertyChoice{Format(u8"{}", pad + 1), pad});
                info.Visible = gamepad;
            }
            else if (field == u8"deadZone")
            {
                info.Description = String(u8"Movement smaller than this reads as none");
                range(0.0, 1.0, 0.01);
                info.Hint = String(u8"slider");
                info.Visible = deadZone;
            }
            else if (field == u8"scale")
            {
                info.DisplayName = String(u8"Scale");
                info.Description = String(u8"Multiplies what the input reports (negative flips it)");
                range(-100.0, 100.0, 0.05);
                info.Visible = source != Source::TouchButton;
            }
            else if (field == u8"invert")
            {
                info.DisplayName = String(u8"Invert");
                info.Description = String(u8"Flip the direction");
                info.Visible = invert;
            }
            else if (field == u8"normalize")
            {
                info.DisplayName = String(u8"Normalize");
                info.Description = String(u8"Diagonals as long as straight moves");
                info.Visible = composite;
            }
            else if (field == u8"regionX" || field == u8"regionY" || field == u8"regionW" || field == u8"regionH")
            {
                info.DisplayName = String(field == u8"regionX" ? StringView(u8"Area X") : field == u8"regionY" ? StringView(u8"Area Y") : field == u8"regionW" ? StringView(u8"Area Width") : StringView(u8"Area Height"));
                info.Description = String(u8"A fraction of the screen, 0 to 1");
                range(0.0, 1.0, 0.01);
                info.Visible = touch;
            }
            else if (field == u8"stickRadius")
            {
                info.DisplayName = String(u8"Radius");
                info.Description = String(u8"How far the stick reaches, in pixels");
                range(0.0, 1000.0, 1.0);
                info.Visible = source == Source::TouchStick;
            }
        }
    }

    namespace input_map_rows
    {
        // An action's processing, named and bounded as the old page had it.
        inline void DescribeProcessorField(StringView field, gui::PropertyInfo& info)
        {
            const auto range = [&info](f64 min, f64 max, f64 step)
            {
                info.HasRange = true;
                info.Min = min;
                info.Max = max;
                info.Step = step;
            };
            if (field == u8"sensitivity")
            {
                info.Description = String(u8"Above 0, a key-driven axis ramps toward its value at this rate a second");
                range(0.0, 100.0, 0.05);
            }
            else if (field == u8"gravity")
            {
                info.Description = String(u8"Above 0, the rate a second it returns to rest once released");
                range(0.0, 100.0, 0.05);
            }
            else if (field == u8"responseExponent")
            {
                info.DisplayName = String(u8"Curve");
                info.Description = String(u8"Shapes an analog input: the value to this power (1 is straight)");
                range(0.1, 5.0, 0.05);
            }
            else if (field == u8"snap")
            {
                info.DisplayName = String(u8"Snap on Reverse");
                info.Description = String(u8"Jump to zero first when the direction flips");
            }
            else if (field == u8"timeScale")
            {
                info.DisplayName = String(u8"Scale with Time");
                info.Description = String(u8"Multiply by the game's time scale, once there is one");
            }
        }
    }

    /// What the page's grid says of `set`'s row `path` (the reflected source over the set), in `info`.
    inline void DescribeInputRow(const input::ActionSet& set, StringView path, gui::PropertyInfo& info)
    {
        if (path == u8"priority")
        {
            info.Description = String(u8"When two active sets bind the same input, the higher priority takes it");
            return;
        }
        if (path == u8"actions")
        {
            info.DisplayName = String(u8"Actions");
            info.Description = String(u8"What the game reads while this set is on; Add adds one to this set");
            return;
        }
        StringView rest;
        const i64 a = input_map_rows::IndexAfter(path, u8"actions.", rest);
        if (a < 0 || static_cast<usize>(a) >= set.actions.Size())
            return;
        const input::Action& action = set.actions[static_cast<usize>(a)];
        if (rest.IsEmpty())
        {
            // An action: by its name; closed, its kind and its bindings in a line.
            info.DisplayName = action.name.IsEmpty() ? String(u8"(unnamed)") : String(action.name.AsView());
            info.Summary = Format(u8"{}: {}", input_map_edit::KindTitle(action.kind), input_map_rows::BindingsLine(action));
            return;
        }
        if (rest == u8"kind")
        {
            info.Description = String(u8"What the action reports: pressed or not, one axis, or two");
            for (gui::PropertyChoice& choice : info.Choices)
                choice.Name = String(input_map_edit::KindTitle(static_cast<input::ActionKind>(choice.Value)));
            return;
        }
        if (rest == u8"processors")
        {
            info.DisplayName = String(u8"Processing");
            info.Summary = Format(u8"Sensitivity {}{}", FormatFixed(action.processors.sensitivity, 2), action.processors.snap ? StringView(u8", snaps on reverse") : StringView{});
            return;
        }
        // When it fires: as soon as pressed, after a hold, on a quick tap or on two (the old page's
        // words for the choices); closed, the choice and its time in a line.
        if (rest == u8"interaction")
        {
            info.DisplayName = String(u8"Interaction");
            info.Description = String(u8"When it fires: as soon as pressed, after a hold, on a quick tap, or on two");
            info.Summary = action.interaction.kind == input::InteractionKind::None
                               ? String(input_map_edit::InteractionTitle(action.interaction.kind))
                               : Format(u8"{} {} {}s", input_map_edit::InteractionTitle(action.interaction.kind),
                                        action.interaction.kind == input::InteractionKind::Hold ? StringView(u8"for") : StringView(u8"within"),
                                        FormatFixed(action.interaction.seconds, 2));
            return;
        }
        if (rest == u8"interaction.kind")
        {
            info.DisplayName = String(u8"Kind");
            for (gui::PropertyChoice& choice : info.Choices)
                choice.Name = String(input_map_edit::InteractionTitle(static_cast<input::InteractionKind>(choice.Value)));
            return;
        }
        if (rest == u8"interaction.seconds")
        {
            // Hold: held for this long; a tap: released, or pressed again, within it.
            info.DisplayName = String(action.interaction.kind == input::InteractionKind::Hold ? StringView(u8"Hold For") : StringView(u8"Within"));
            info.Description = String(u8"Seconds");
            info.HasRange = true;
            info.Min = 0.0;
            info.Max = 10.0;
            info.Step = 0.05;
            info.Visible = action.interaction.kind != input::InteractionKind::None;
            return;
        }
        if (rest == u8"bindings")
        {
            info.DisplayName = String(u8"Bindings");
            info.Description = String(u8"The inputs that drive it; Add binds another");
            return;
        }
        const StringView processors = u8"processors.";
        if (rest.StartsWith(processors))
        {
            input_map_rows::DescribeProcessorField(rest.SubStr(processors.Size(), rest.Size() - processors.Size()), info);
            return;
        }
        StringView field;
        const i64 b = input_map_rows::IndexAfter(rest, u8"bindings.", field);
        if (b < 0 || static_cast<usize>(b) >= action.bindings.Size())
            return;
        const input::Binding& binding = action.bindings[static_cast<usize>(b)];
        if (field.IsEmpty())
        {
            // A binding: by its source; closed, its input, and when the action's kind does not take
            // its source (a map the game cannot read), that said on its own row.
            info.DisplayName = String(input_map_edit::SourceTitle(binding.source));
            info.Summary = input::DescribeBinding(binding);
            input::BindingSource valid[8];
            const usize count = input::ValidSources(action.kind, valid);
            bool taken = false;
            for (usize i = 0; i < count; ++i)
                taken = taken || valid[i] == binding.source;
            if (!taken)
                info.Summary += Format(u8"  (not taken by a {} action)", input_map_edit::KindTitle(action.kind)).AsView();
            return;
        }
        input_map_rows::DescribeBindingField(binding, field, info);
        if (field == u8"source")
        {
            // Only the sources this action's kind takes.
            input::BindingSource valid[8];
            const usize count = input::ValidSources(action.kind, valid);
            Array<gui::PropertyChoice> kept;
            for (const gui::PropertyChoice& choice : info.Choices)
                for (usize i = 0; i < count; ++i)
                    if (static_cast<i64>(valid[i]) == choice.Value || static_cast<i64>(binding.source) == choice.Value)
                    {
                        kept.PushBack(choice);
                        break;
                    }
            info.Choices = Move(kept);
        }
    }
}
