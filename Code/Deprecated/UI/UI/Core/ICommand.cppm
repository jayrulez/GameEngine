// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :icommand partition
//
// ICommand: MVVM-style command binding on controls. ButtonBase.Command executes this
// when clicked if CanExecute() is true. Ported from Sedulous.UI/src/Core/ICommand.bf.
// Injected/held-by-reference (pattern B) - implemented by non-View app command objects.

export module foundation.ui:icommand;

export namespace foundation::ui
{
    class ICommand
    {
    public:
        virtual ~ICommand() = default;

        /// Whether the command can currently execute.
        [[nodiscard]] virtual bool CanExecute() = 0;
        /// Execute the command.
        virtual void Execute() = 0;
    };
}
