// Draconic UI - :iclipboard partition
//
// IClipboard: clipboard operations, defined in the UI layer to avoid a shell dependency
// (the core stays platform-agnostic). The app / ui.shell bridge supplies an adapter that
// bridges the platform clipboard. Ported from Sedulous.UI/src/Core/IClipboard.bf.
//
// Injected/held-by-reference (pattern B). Beef Result<void> -> foundation::Status; Beef HasText
// property -> HasText() method.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:iclipboard;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class IClipboard
    {
    public:
        virtual ~IClipboard() = default;

        /// Gets text from the clipboard into outText.
        [[nodiscard]] virtual Status GetText(String& outText) = 0;
        /// Sets clipboard text.
        [[nodiscard]] virtual Status SetText(StringView text) = 0;
        /// Whether the clipboard currently contains text.
        [[nodiscard]] virtual bool HasText() = 0;
    };
}
