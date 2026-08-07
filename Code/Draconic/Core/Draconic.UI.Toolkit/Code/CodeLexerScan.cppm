// Draconic UI Toolkit - :code_lexer_scan partition (module-internal).
//
// Shared scanning primitives for the concrete lexers (CLikeLexer.cpp, XmlLexer.cpp): a
// byte+codepoint cursor, character classes, and the token emitter. An INTERNAL partition
// (partition implementation unit): importable by this module's other units, invisible to
// toolkit consumers - GCC rejects the not-re-exported interface-partition alternative.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.ui.toolkit:code_lexer_scan;

import draconic.foundation;
import :code_lexer;

using namespace draconic::foundation;

namespace draconic::ui::toolkit::lexer_scan
{
    [[nodiscard]] constexpr bool IsDigit(char8_t cursor) noexcept { return cursor >= u8'0' && cursor <= u8'9'; }
    [[nodiscard]] constexpr bool IsIdentStart(char8_t cursor) noexcept
    {
        return (cursor >= u8'a' && cursor <= u8'z') || (cursor >= u8'A' && cursor <= u8'Z') || cursor == u8'_' || cursor >= 0x80;
    }
    [[nodiscard]] constexpr bool IsIdentChar(char8_t cursor) noexcept
    {
        return IsIdentStart(cursor) || IsDigit(cursor);
    }
    [[nodiscard]] constexpr bool IsSpace(char8_t cursor) noexcept { return cursor == u8' ' || cursor == u8'\t'; }

    /// Cursor that tracks the codepoint column alongside the byte index.
    struct Cursor
    {
        StringView text;
        usize i = 0;
        i32 column = 0;

        [[nodiscard]] bool AtEnd() const noexcept { return i >= text.Size(); }
        [[nodiscard]] char8_t Peek(usize ahead = 0) const noexcept
        {
            return i + ahead < text.Size() ? text[i + ahead] : char8_t(0);
        }
        [[nodiscard]] bool Match(const char8_t* s, usize n) const noexcept
        {
            if (i + n > text.Size())
            {
                return false;
            }
            for (usize k = 0; k < n; ++k)
            {
                if (text[i + k] != s[k])
                {
                    return false;
                }
            }
            return true;
        }
        /// One codepoint: ASCII fast path, else skip the multibyte sequence.
        void Advance() noexcept
        {
            if (AtEnd())
            {
                return;
            }
            ++i;
            while (i < text.Size() && IsUtf8Continuation(text[i]))
            {
                ++i;
            }
            ++column;
        }
    };

    inline void Emit(Array<CodeToken>& out, usize byteBegin, usize byteEnd, i32 column,
                     CodeTokenKind kind)
    {
        if (byteEnd > byteBegin)
        {
            out.PushBack(
                CodeToken{static_cast<u32>(byteBegin), static_cast<u32>(byteEnd), column, kind});
        }
    }
}
