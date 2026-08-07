// Draconic::ShellWeb - `draconic.shell.web:dialogs`.
//
// The web shell's file-dialog service. Stubbed to cancel immediately; the browser equivalents are a
// hidden <input type=file> (open) and an anchor-download (save), wired here later. Kept as the web
// shell's own service so that wiring has a home.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shell.web:dialogs;

import draconic.foundation;
import draconic.shell;

namespace foundation = draconic::foundation;

export namespace draconic::shell
{
    class WebDialogService final : public IDialogService
    {
    public:
        void ShowOpenFile(DialogResultCallback callback, foundation::Span<const FileFilter> = {},
                          foundation::StringView = {}, bool = false, foundation::u32 = 0) override
        {
            Cancel(callback);
        }
        void ShowSaveFile(DialogResultCallback callback, foundation::Span<const FileFilter> = {},
                          foundation::StringView = {}, foundation::u32 = 0) override
        {
            Cancel(callback);
        }
        void ShowOpenFolder(DialogResultCallback callback, foundation::StringView = {}, bool = false,
                            foundation::u32 = 0) override
        {
            Cancel(callback);
        }
        void OpenPath(foundation::StringView) override {} // no OS file manager in a browser

    private:
        static void Cancel(DialogResultCallback& callback)
        {
            if (callback)
            {
                callback(foundation::Span<const foundation::String>{});
            }
        }
    };
}
