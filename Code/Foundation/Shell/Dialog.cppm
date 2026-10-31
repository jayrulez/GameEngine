// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Shell - :dialog partition.
//
// Native OS file and folder dialogs. They are asynchronous: the Show* call returns immediately and
// the callback fires later (during the shell's event pump), exactly once, on the main thread, so
// unlike the rest of the shell's pull-based event model these carry a callback. The desktop backend
// wraps SDL3's SDL_ShowOpenFileDialog, SDL_ShowSaveFileDialog and SDL_ShowOpenFolderDialog.

module;
#include "Core/Prelude.h"

export module foundation.shell:dialog;

import foundation.core;

namespace core = foundation::core;

export namespace foundation::shell
{
    // A file-type filter for open/save dialogs. `pattern` is a semicolon-separated list of
    // extensions without dots (the SDL form): name = u8"Images", pattern = u8"png;jpg;jpeg".
    // pattern = u8"*" matches everything. Both are borrowed for the duration of the Show* call.
    struct FileFilter
    {
        core::StringView name;
        core::StringView pattern;
    };

    // Delivered once when a dialog resolves. `paths` is empty on cancel or error; otherwise one
    // entry (or several, for a multi-select open). The core::String elements are valid for the
    // duration of the call; copy any you need to keep past it.
    using DialogResultCallback = core::Function<void(core::Span<const core::String>)>;

    // Native OS file and folder dialogs, and shell "open" actions. Every Show* is async: it returns
    // immediately and `callback` fires later, exactly once, on the main thread (during the event
    // pump). Headless and null backends invoke the callback immediately with an empty result (treated
    // as cancel), so callers need no backend check.
    class IDialogService
    {
    public:
        virtual ~IDialogService() = default;

        /// Show a native "open file" dialog. `filters` restrict the selectable types (empty means all
        /// files); `defaultPath` is the initial directory or file to focus; `allowMultiple` permits
        /// selecting several files; `parentWindowId` (0 for none) makes the dialog modal to that window
        /// where the backend supports it. Async: `callback` fires once with the chosen path(s), or an
        /// empty span on cancel or error.
        virtual void ShowOpenFile(DialogResultCallback callback,
                                  core::Span<const FileFilter> filters = {},
                                  core::StringView defaultPath = {}, bool allowMultiple = false,
                                  core::u32 parentWindowId = 0) = 0;

        /// Show a native "save file" dialog to pick a (possibly new) file path to write. Same
        /// `filters`, `defaultPath`, `parentWindowId` and async-callback semantics as ShowOpenFile,
        /// but always a single path (empty span on cancel).
        virtual void ShowSaveFile(DialogResultCallback callback,
                                  core::Span<const FileFilter> filters = {},
                                  core::StringView defaultPath = {},
                                  core::u32 parentWindowId = 0) = 0;

        /// Show a native "choose folder" dialog. `defaultPath` is the initial directory;
        /// `allowMultiple` permits selecting several folders; `parentWindowId` as above. Async:
        /// `callback` fires once with the chosen folder(s), or an empty span on cancel or error.
        virtual void ShowOpenFolder(DialogResultCallback callback,
                                    core::StringView defaultPath = {}, bool allowMultiple = false,
                                    core::u32 parentWindowId = 0) = 0;

        /// Open `path` with the OS default handler: a folder opens in the system file manager, a file
        /// opens in its associated application. Fire-and-forget (no callback, no result). A no-op on
        /// headless backends. Used, for example, to reveal an export's output directory.
        virtual void OpenPath(core::StringView path) = 0;
    };
}
