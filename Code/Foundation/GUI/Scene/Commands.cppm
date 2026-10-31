// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :commands partition
//
// The command model. A command has an id, a label, an icon, and enabled and checked states.
// Buttons, menu items, shortcuts and gamepad buttons refer to it by id, so disabling the command
// greys every control bound to it. Dispatch follows GPUI's (`zed/crates/gpui/src/key_dispatch.rs`):
// running a command walks the focus path, first the capture handlers from the root down, then the
// bubble handlers from the focused widget up. A bubble handler stops the command unless it sets
// Propagate. A command is available (its controls enabled) when it is enabled and a handler for it
// is on the focus path, so moving focus enables and greys the right controls without bookkeeping.
// Availability is observable: a control's binding follows the focus, the command's enabled state
// and the handlers.

module;
#include "Core/Prelude.h"

export module foundation.gui:commands;

import foundation.core;
import :reactive;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class Command
    {
    public:
        explicit Command(core::StringView id) : m_id(id) {}
        Command(const Command&) = delete;
        Command& operator=(const Command&) = delete;

        [[nodiscard]] core::StringView GetId() const noexcept { return m_id.AsView(); }
        [[nodiscard]] core::StringView GetLabel() const { return m_label.Get().AsView(); }
        void SetLabel(core::StringView label) { m_label.Set(core::String(label)); }
        [[nodiscard]] core::StringView GetIcon() const { return m_icon.Get().AsView(); }
        void SetIcon(core::StringView icon) { m_icon.Set(core::String(icon)); }
        /// A binding that reads this follows changes to it.
        [[nodiscard]] bool IsEnabled() const { return m_enabled.Get(); }
        void SetEnabled(bool enabled) { m_enabled.Set(enabled); }
        [[nodiscard]] bool IsChecked() const { return m_checked.Get(); }
        void SetChecked(bool checked) { m_checked.Set(checked); }
        /// Why the command is disabled ("Nothing to save"). A control bound to it shows this while
        /// the command is disabled (as a tool bar button's tooltip). A binding that reads this
        /// follows changes to it.
        [[nodiscard]] core::StringView GetDisabledReason() const { return m_disabledReason.Get().AsView(); }
        void SetDisabledReason(core::StringView reason) { m_disabledReason.Set(core::String(reason)); }

    private:
        core::String m_id;
        Property<core::String> m_label;
        Property<core::String> m_icon;
        Property<bool> m_enabled{true};
        Property<bool> m_checked{false};
        Property<core::String> m_disabledReason;
    };

    /// When a node's handler runs: on the way down from the root (capture), or on the way up from
    /// the focused widget (bubble, the usual choice).
    enum class CommandPhase : u8
    {
        Capture,
        Bubble,
    };

    /// What a handler receives: the command, and flags that let it pass on or stop the command.
    struct CommandEvent
    {
        core::StringView Id;
        CommandPhase Phase = CommandPhase::Bubble;
        bool Propagate = false; ///< A bubble handler sets it to pass the command on up.
        bool Stop = false;      ///< A capture handler sets it to take the command: no handler below it and no bubble handler runs.
    };

    using CommandHandler = core::Function<void(CommandEvent&)>;

    /// The command state passed to a control bound to the command (Node::OnBoundCommandState).
    struct BoundCommandState
    {
        bool Available = false; ///< The command is enabled and a handler for it is on the focus path.
        bool Checked = false;
        /// Why the command is not available: its disabled reason while it is disabled, else empty
        /// (a missing handler at the focus is not something the user could act on).
        core::StringView Reason;
    };

    /// A GUI's commands, by id.
    class CommandSet
    {
    public:
        CommandSet() noexcept = default;
        CommandSet(const CommandSet&) = delete;
        CommandSet& operator=(const CommandSet&) = delete;

        /// Returns the command `id`, defining it if it is new (enabled, with the id as its label
        /// until one is set).
        Command& Define(core::StringView id, IAllocator& allocator)
        {
            if (Command* found = Find(id))
                return *found;
            m_commands.PushBack(MakeUnique<Command>(allocator, id));
            Command& command = *m_commands[m_commands.Size() - 1];
            command.SetLabel(id);
            m_changed.Notify();
            return command;
        }
        [[nodiscard]] Command* Find(core::StringView id) const
        {
            for (const UniquePtr<Command>& command : m_commands)
                if (command->GetId() == id)
                    return command.Get();
            return nullptr;
        }
        /// Tells the bindings that something affecting availability changed (a definition, or a
        /// handler added or removed).
        void NotifyChanged() { m_changed.Notify(); }
        void TrackChanges() const { m_changed.Track(); }

    private:
        Array<UniquePtr<Command>> m_commands;
        Observable m_changed;
    };
}
