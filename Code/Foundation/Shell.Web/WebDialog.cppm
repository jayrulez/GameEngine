// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Shell.Web - `foundation.shell.web:dialogs`.
//
// The web shell's file-dialog service. It cancels immediately; the browser equivalents would be a
// hidden <input type=file> (open) and an anchor download (save). It is the web shell's own service
// so that wiring has a home.

module;
#include "Core/Prelude.h"

export module foundation.shell.web:dialogs;

import foundation.core;
import foundation.shell;

namespace core = foundation::core;

export namespace foundation::shell
{
    class WebDialogService final : public IDialogService
    {
    public:
        void ShowOpenFile(DialogResultCallback callback, core::Span<const FileFilter> = {},
                          core::StringView = {}, bool = false, core::u32 = 0) override
        {
            Cancel(callback);
        }
        void ShowSaveFile(DialogResultCallback callback, core::Span<const FileFilter> = {},
                          core::StringView = {}, core::u32 = 0) override
        {
            Cancel(callback);
        }
        void ShowOpenFolder(DialogResultCallback callback, core::StringView = {}, bool = false,
                            core::u32 = 0) override
        {
            Cancel(callback);
        }
        void OpenPath(core::StringView) override {} // no OS file manager in a browser

    private:
        static void Cancel(DialogResultCallback& callback)
        {
            if (callback)
            {
                callback(core::Span<const core::String>{});
            }
        }
    };
}
