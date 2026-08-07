// Draconic UI - :itext_edit_host partition
//
// Interface that TextEditingBehavior uses to talk to its host control (EditText). The host owns
// the text, handles font/shaping, and fires events. Ported from Sedulous.UI/src/Editing/ITextEditHost.bf.
//
// Pattern-B interface (held-by-known-reference / injected), so it is a plain abstract base with pure
// virtuals (no As*() tree query). Beef read-only properties become getter methods. NAMING DIVERGENCE:
// two accessors are `Get`-prefixed - GetMaxLength()/GetIsReadOnly() - because EditText exposes
// identically-named `Property<>` fields (MaxLength, IsReadOnly) and a C++ class cannot have a data
// member and a member function of the same name (same precedent as UIContext::GetStyleSheet). The
// non-colliding accessors keep their faithful names.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:itext_edit_host;

import draconic.foundation; // StringView
import :iclipboard;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class ITextEditHost
    {
    public:
        virtual ~ITextEditHost() = default;

        /// The current text content (read-only view).
        [[nodiscard]] virtual StringView Text() const = 0;

        /// The maximum allowed text length in characters (0 = unlimited).
        [[nodiscard]] virtual i32 GetMaxLength() const = 0;

        /// Whether the control is read-only.
        [[nodiscard]] virtual bool GetIsReadOnly() const = 0;

        /// Whether multiline editing is enabled.
        [[nodiscard]] virtual bool IsMultiline() const = 0;

        /// The number of characters in the text (not bytes).
        [[nodiscard]] virtual i32 TextCharCount() const = 0;

        /// Replace a range of characters. charStart / charLength are character indices, not byte
        /// offsets; the host converts internally.
        virtual void ReplaceText(i32 charStart, i32 charLength, StringView replacement) = 0;

        /// Notify the host that text content changed (fire events, re-shape).
        virtual void OnTextModified() = 0;

        /// Hit-test: character insertion index at local coordinates.
        [[nodiscard]] virtual i32 HitTestPosition(f32 localX, f32 localY) = 0;

        /// Hit-test in glyph space (no padding/scroll adjustment). Used for Up/Down line navigation.
        [[nodiscard]] virtual i32 HitTestGlyphPosition(f32 glyphX, f32 glyphY) = 0;

        /// X pixel position of the cursor at the given character index.
        [[nodiscard]] virtual f32 GetCursorXPosition(i32 charIndex) = 0;

        /// Y pixel position (top of line) for the given character index (multiline Up/Down nav).
        [[nodiscard]] virtual f32 GetCursorYPosition(i32 charIndex) = 0;

        /// Line height in pixels.
        [[nodiscard]] virtual f32 LineHeight() = 0;

        /// Clipboard adapter (may be null if not available).
        [[nodiscard]] virtual IClipboard* Clipboard() = 0;

        /// Current time in seconds (for undo coalescing).
        [[nodiscard]] virtual f32 CurrentTime() = 0;
    };
}
