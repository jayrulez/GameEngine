// Draconic::Shell - :dialog partition.
//
// Native OS file/folder dialogs. Async by nature - the Show* call returns immediately and the
// callback fires later (during the shell's event pump), exactly once, on the main thread - so unlike
// the rest of the shell's pull-based event model these carry a callback. Modeled on Sedulous's
// IDialogService; the desktop backend wraps SDL3's SDL_Show{Open,Save}FileDialog /
// SDL_ShowOpenFolderDialog. A future in-engine file-browser widget can layer on top; native first.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shell:dialog;

import draconic.foundation;

namespace foundation = draconic::foundation;

export namespace draconic::shell
{
    // A file-type filter for open/save dialogs. `pattern` is a semicolon-separated list of
    // extensions WITHOUT dots (the SDL form): name = u8"Images", pattern = u8"png;jpg;jpeg".
    // pattern = u8"*" matches everything. Both are borrowed for the duration of the Show* call.
    struct FileFilter
    {
        foundation::StringView name;
        foundation::StringView pattern;
    };

    // Delivered once when a dialog resolves. `paths` is empty on cancel or error; otherwise one
    // entry (or several, for a multi-select open). The foundation::String elements are valid for the
    // duration of the call - copy any you need to keep past it. (Improvement over Sedulous, which
    // hands back views into SDL's transient list.)
    using DialogResultCallback = foundation::Function<void(foundation::Span<const foundation::String>)>;

    // Native OS file/folder dialogs + shell "open" actions. Every Show* is async: it returns
    // immediately and `callback` fires later, exactly once, on the main thread (during the event
    // pump). Headless / null backends invoke the callback immediately with an empty result (treated as
    // cancel), so callers need no backend check.
    class IDialogService
    {
    public:
        virtual ~IDialogService() = default;

        /// Show a native "open file" dialog. `filters` restrict the selectable types (empty => all
        /// files); `defaultPath` is the initial directory or file to focus; `allowMultiple` permits
        /// selecting several files; `parentWindowId` (0 => none) makes the dialog modal to that window
        /// where the backend supports it. Async: `callback` fires once with the chosen path(s), or an
        /// empty span on cancel / error.
        virtual void ShowOpenFile(DialogResultCallback callback,
                                  foundation::Span<const FileFilter> filters = {},
                                  foundation::StringView defaultPath = {}, bool allowMultiple = false,
                                  foundation::u32 parentWindowId = 0) = 0;

        /// Show a native "save file" dialog to pick a (possibly new) file path to write. Same
        /// `filters` / `defaultPath` / `parentWindowId` / async-callback semantics as ShowOpenFile,
        /// but always a single path (empty span on cancel).
        virtual void ShowSaveFile(DialogResultCallback callback,
                                  foundation::Span<const FileFilter> filters = {},
                                  foundation::StringView defaultPath = {},
                                  foundation::u32 parentWindowId = 0) = 0;

        /// Show a native "choose folder" dialog. `defaultPath` is the initial directory;
        /// `allowMultiple` permits selecting several folders; `parentWindowId` as above. Async:
        /// `callback` fires once with the chosen folder(s), or an empty span on cancel / error.
        virtual void ShowOpenFolder(DialogResultCallback callback,
                                    foundation::StringView defaultPath = {}, bool allowMultiple = false,
                                    foundation::u32 parentWindowId = 0) = 0;

        /// Open `path` with the OS default handler: a FOLDER opens in the system file manager, a file
        /// opens in its associated application. Fire-and-forget (no callback / no result). No-op on
        /// headless backends. Used e.g. to reveal an export's output directory.
        virtual void OpenPath(foundation::StringView path) = 0;
    };
}
