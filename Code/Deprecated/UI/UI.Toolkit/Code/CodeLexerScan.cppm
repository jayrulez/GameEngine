// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :code_lexer_scan partition (module-internal).
//
// Shared scanning primitives for the concrete lexers (CLikeLexer.cpp, XmlLexer.cpp): a
// byte+codepoint cursor, character classes, and the token emitter. An INTERNAL partition
// (partition implementation unit): importable by this module's other units, invisible to
// toolkit consumers - GCC rejects the not-re-exported interface-partition alternative.

module;
#include "Core/Prelude.h"

module foundation.ui.toolkit:code_lexer_scan;

import foundation.core;
import :code_lexer;

using namespace foundation::core;

namespace foundation::ui::toolkit::lexer_scan
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

    // ---- token scanners shared by the concrete lexers (CLikeLexer, LuaLikeLexer) --------------

    [[nodiscard]] constexpr bool IsPunctuationChar(char8_t character) noexcept
    {
        return character == u8'(' || character == u8')' || character == u8'[' ||
               character == u8']' || character == u8'{' || character == u8'}' ||
               character == u8',' || character == u8';' || character == u8'.';
    }

    /// Single-line quoted literal with backslash escapes; unterminated = rest of line.
    inline void ScanQuoted(Cursor& cursor, Array<CodeToken>& out, char8_t quote)
    {
        const usize begin = cursor.i;
        const i32 column = cursor.column;
        cursor.Advance(); // the opening quote
        while (!cursor.AtEnd())
        {
            const char8_t character = cursor.Peek();
            if (character == u8'\\')
            {
                cursor.Advance();
                cursor.Advance();
                continue;
            }
            if (character == quote)
            {
                cursor.Advance();
                break;
            }
            cursor.Advance();
        }
        Emit(out, begin, cursor.i, column, CodeTokenKind::String);
    }

    /// Loose number scan (ints, floats, hex, exponents, suffixes) - built for coloring,
    /// not validation.
    inline void ScanNumber(Cursor& cursor, Array<CodeToken>& out)
    {
        const usize begin = cursor.i;
        const i32 column = cursor.column;
        char8_t previous = 0;
        while (!cursor.AtEnd())
        {
            const char8_t character = cursor.Peek();
            const bool exponentSign = (character == u8'+' || character == u8'-') &&
                                      (previous == u8'e' || previous == u8'E');
            if (!(IsIdentChar(character) || character == u8'.' || exponentSign))
            {
                break;
            }
            previous = character;
            cursor.Advance();
        }
        Emit(out, begin, cursor.i, column, CodeTokenKind::Number);
    }

    /// Identifier run, classified against the owner's keyword/type tables (else Default).
    inline void ScanWord(Cursor& cursor, Array<CodeToken>& out, const HashSet<u64>& keywords,
                         const HashSet<u64>& types)
    {
        const usize begin = cursor.i;
        const i32 column = cursor.column;
        while (!cursor.AtEnd() && IsIdentChar(cursor.Peek()))
        {
            cursor.Advance();
        }
        const StringView word = cursor.text.SubStr(begin, cursor.i - begin);
        const u64 hash = HashBytes(word.Data(), word.Size());
        CodeTokenKind kind = CodeTokenKind::Default;
        if (keywords.Contains(hash))
        {
            kind = CodeTokenKind::Keyword;
        }
        else if (types.Contains(hash))
        {
            kind = CodeTokenKind::Type;
        }
        Emit(out, begin, cursor.i, column, kind);
    }
}
