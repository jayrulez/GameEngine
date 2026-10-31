// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :icommand partition
//
// ICommand: MVVM-style command binding on controls. ButtonBase.Command executes this
// when clicked if CanExecute() is true. Injected and held by reference; implemented by
// app command objects that are not Views.

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
