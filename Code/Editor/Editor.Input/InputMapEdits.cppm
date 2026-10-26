// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Input - :input_map_edits partition.
//
// The input map's edits without a page: what the page's rows and its input capture do to the map,
// every lookup guarded (an out-of-range set, action or binding is a no-op or null, since a row can
// outlive the map it was read from). Kept apart from the page (Documentation/Specs/editor-gui.md,
// 5.4, layer 5) so its tests need no GUI.

module;
#include "Core/Prelude.h"

export module editor.input:input_map_edits;

import foundation.core;
import foundation.input;

using namespace foundation::core;

export namespace editor
{
    namespace input = foundation::input;

    namespace detail
    {
        // Binding + enum labels now live in foundation.input (:binding_names); re-exported here so
        // this page and its impl keep calling them as detail::<name>.
        using input::DescribeBinding;
        using input::InteractionName;
        using input::KeyName;
        using input::KindName;
        using input::PadButtonName;
        using input::SourceName;
        using input::ValidSources;
    }

    /// The input map editor's headless edits: what the rows' clicks and the listen capture do
    /// to the map, without a page. Every lookup is guarded (an out-of-range set, action or
    /// binding is a no-op or null), since a row can outlive the map it was built from.
    namespace input_map_edit
    {
        [[nodiscard]] inline input::ActionSet* SetAt(input::InputMap& map, usize set)
        {
            return set < map.sets.Size() ? &map.sets[set] : nullptr;
        }
        [[nodiscard]] inline input::Action* ActionAt(input::InputMap& map, usize set, usize action)
        {
            input::ActionSet* s = SetAt(map, set);
            return (s != nullptr && action < s->actions.Size()) ? &s->actions[action] : nullptr;
        }
        [[nodiscard]] inline bool HasBinding(input::InputMap& map, usize set, usize action,
                                             usize binding)
        {
            const input::Action* a = ActionAt(map, set, action);
            return a != nullptr && binding < a->bindings.Size();
        }
        /// A new binding for an action of `kind`: a stick for a 2D axis, else a key.
        [[nodiscard]] inline input::Binding FreshBinding(input::ActionKind kind)
        {
            input::Binding fresh;
            if (kind == input::ActionKind::Axis2D)
            {
                fresh.source = input::BindingSource::GamepadStick;
            }
            return fresh;
        }
        /// The next valid source for the kind after `current`'s (wrapping), on a FRESH binding:
        /// a source change resets the source-specific fields.
        [[nodiscard]] inline input::Binding CycleSource(input::ActionKind kind,
                                                        const input::Binding& current)
        {
            input::BindingSource valid[8];
            const usize n = input::ValidSources(kind, valid);
            usize index = 0;
            for (usize i = 0; i < n; ++i)
            {
                if (valid[i] == current.source)
                {
                    index = i;
                    break;
                }
            }
            input::Binding fresh;
            fresh.source = valid[(index + 1) % n];
            return fresh;
        }
        /// A fresh binding of `source`, for the Add Binding menu.
        [[nodiscard]] inline input::Binding FreshBinding(input::BindingSource source)
        {
            input::Binding fresh;
            fresh.source = source;
            return fresh;
        }
        /// A source as the page names it: what the user plugs in or touches.
        [[nodiscard]] inline StringView SourceTitle(input::BindingSource source)
        {
            switch (source)
            {
            case input::BindingSource::Key:
                return u8"Key";
            case input::BindingSource::MouseButton:
                return u8"Mouse button";
            case input::BindingSource::MouseAxis:
                return u8"Mouse axis";
            case input::BindingSource::MouseDelta:
                return u8"Mouse movement";
            case input::BindingSource::GamepadButton:
                return u8"Gamepad button";
            case input::BindingSource::GamepadAxis:
                return u8"Gamepad axis";
            case input::BindingSource::GamepadStick:
                return u8"Gamepad stick";
            case input::BindingSource::Composite2D:
                return u8"Four keys";
            case input::BindingSource::TouchButton:
                return u8"Touch button";
            case input::BindingSource::TouchStick:
                return u8"Touch stick";
            }
            return u8"?";
        }
        /// An action kind as the page names it.
        [[nodiscard]] inline StringView KindTitle(input::ActionKind kind)
        {
            switch (kind)
            {
            case input::ActionKind::Button:
                return u8"Button";
            case input::ActionKind::Axis1D:
                return u8"1D axis";
            case input::ActionKind::Axis2D:
                return u8"2D axis";
            }
            return u8"?";
        }

        /// When an action fires, as the page names it (the old page's words).
        [[nodiscard]] inline StringView InteractionTitle(input::InteractionKind kind)
        {
            switch (kind)
            {
            case input::InteractionKind::None:
                return u8"On press";
            case input::InteractionKind::Hold:
                return u8"Hold";
            case input::InteractionKind::Tap:
                return u8"Tap";
            case input::InteractionKind::DoubleTap:
                return u8"Double tap";
            }
            return u8"?";
        }

        /// What a listen for an action of `kind` captures: a Button rebind ignores stick
        /// noise, an Axis2D rebind captures sticks only; one Composite2D DIRECTION is always a
        /// single key.
        [[nodiscard]] inline input::CaptureFilter FilterFor(input::ActionKind kind,
                                                            bool compositeDirection)
        {
            input::CaptureFilter filter;
            if (compositeDirection)
            {
                filter.mouseButtons = false;
                filter.gamepadButtons = false;
                return filter;
            }
            switch (kind)
            {
            case input::ActionKind::Button:
                break; // keys + mouse + pad buttons
            case input::ActionKind::Axis1D:
                filter.gamepadAxes = true;
                break;
            case input::ActionKind::Axis2D:
                filter.keys = false;
                filter.mouseButtons = false;
                filter.gamepadButtons = false;
                filter.gamepadSticks = true;
                break;
            }
            return filter;
        }
        /// Lands a capture: the whole binding, or (direction 0..3 = -X +X -Y +Y) just the
        /// captured KEY code into that Composite2D slot. Out-of-range targets are ignored.
        inline void ApplyCapture(input::InputMap& map, usize set, usize action, usize binding,
                                 i32 direction, const input::Binding& captured)
        {
            input::Action* a = ActionAt(map, set, action);
            if (a == nullptr || binding >= a->bindings.Size())
            {
                return;
            }
            input::Binding& target = a->bindings[binding];
            if (direction < 0)
            {
                target = captured;
                return;
            }
            u32* slot = direction == 0   ? &target.negX
                        : direction == 1 ? &target.posX
                        : direction == 2 ? &target.negY
                                         : &target.posY;
            *slot = captured.code;
        }
    }
}
