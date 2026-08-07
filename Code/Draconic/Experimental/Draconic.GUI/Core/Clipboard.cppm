// Draconic GUI - :clipboard partition
//
// IClipboard: the GUI core's abstract system-clipboard seam. Text widgets (TextField)
// cut/copy/paste through this interface, so the core stays platform-agnostic - exactly
// like the platform text-input (IME) seam. The concrete implementation lives in the
// gui.shell bridge (ShellClipboard, wrapping shell::IShell's clipboard); tests supply a
// trivial in-memory one. The dispatcher holds a non-owning pointer a widget reaches via
// GetEventDispatcher()->GetClipboard(), which may be null (then cut/copy/paste no-op).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:clipboard;

import draconic.foundation; // String, StringView

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class IClipboard
    {
    public:
        virtual ~IClipboard() = default;

        // True if the clipboard currently holds text.
        [[nodiscard]] virtual bool HasText() const = 0;

        // The clipboard's text (empty string if none).
        [[nodiscard]] virtual foundation::String GetText() const = 0;

        // Replace the clipboard's text.
        virtual void SetText(foundation::StringView text) = 0;
    };
}
