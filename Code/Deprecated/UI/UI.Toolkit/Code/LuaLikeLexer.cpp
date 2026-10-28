// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - LuaLikeLexer implementation (declared in :code_lexer).
//
// The Lua-family scanner (Luau). Language identity - keyword/type tables - lives in the
// LuaLikeLexerSpec the OWNER supplies; the toolkit ships no tables (see CodeLexer.cppm). What
// the C-like scanner cannot express and this one adds: `--` line comments, level-bracketed long
// comments `--[[ ]]` / `--[=[ ]=]`, long strings `[[ ]]` / `[=[ ]=]`, and backtick interpolated
// strings. Long brackets are multi-line; the carry state encodes (mode | level << 8) so an
// unterminated long string/comment resumes on the next line at the same level.

module;
#include "Core/Prelude.h"

module foundation.ui.toolkit;

import foundation.core;
import :code_lexer_scan;

using namespace foundation::core;

namespace foundation::ui::toolkit
{
    namespace
    {
        using lexer_scan::Cursor;
        using lexer_scan::Emit;
        using lexer_scan::IsPunctuationChar;
        using lexer_scan::ScanNumber;
        using lexer_scan::ScanQuoted;
        using lexer_scan::ScanWord;

        constexpr u32 kModeLongString = 1;
        constexpr u32 kModeLongComment = 2;

        /// At a '[', is this the open of a long bracket `[` `=`*N `[`? Returns N (>= 0) without
        /// advancing, or -1 if not a long-bracket open. (Also used for the `]` `=`*N `]` close.)
        [[nodiscard]] int LongBracketLevel(const Cursor& cursor, char8_t bracket)
        {
            if (cursor.Peek() != bracket)
            {
                return -1;
            }
            usize level = 0;
            while (cursor.Peek(1 + level) == u8'=')
            {
                ++level;
            }
            return (cursor.Peek(1 + level) == bracket) ? static_cast<int>(level) : -1;
        }

        /// Scan inside a long bracket of `level` until its matching close `]` `=`*level `]`.
        /// Returns 0 when closed on this line, else the carry state (mode | level << 8). The
        /// opening bracket must already be consumed; tokenBegin/Column mark the token start.
        u32 ScanLongBracket(Cursor& cursor, Array<CodeToken>& out, u32 level, CodeTokenKind kind,
                            u32 mode, usize tokenBegin, i32 tokenColumn)
        {
            while (!cursor.AtEnd())
            {
                if (cursor.Peek() == u8']' &&
                    LongBracketLevel(cursor, u8']') == static_cast<int>(level))
                {
                    for (u32 n = 0; n < level + 2; ++n) // ']' + level '=' + ']'
                    {
                        cursor.Advance();
                    }
                    Emit(out, tokenBegin, cursor.i, tokenColumn, kind);
                    return 0;
                }
                cursor.Advance();
            }
            Emit(out, tokenBegin, cursor.i, tokenColumn, kind);
            return mode | (level << 8);
        }
    }

    LuaLikeLexer::LuaLikeLexer(const LuaLikeLexerSpec& spec)
    {
        for (usize i = 0; i < spec.keywords.Size(); ++i)
        {
            m_keywords.Insert(HashBytes(spec.keywords[i].Data(), spec.keywords[i].Size()));
        }
        for (usize i = 0; i < spec.types.Size(); ++i)
        {
            m_types.Insert(HashBytes(spec.types[i].Data(), spec.types[i].Size()));
        }
    }

    u32 LuaLikeLexer::LexLine(StringView line, u32 entryState, Array<CodeToken>& out)
    {
        Cursor cursor{line};

        // Resume a multi-line long string / long comment at its level.
        const u32 mode = entryState & 0xFFu;
        if (mode == kModeLongString || mode == kModeLongComment)
        {
            const CodeTokenKind kind =
                (mode == kModeLongString) ? CodeTokenKind::String : CodeTokenKind::Comment;
            const u32 state = ScanLongBracket(cursor, out, entryState >> 8, kind, mode, 0, 0);
            if (state != 0)
            {
                return state;
            }
        }

        while (!cursor.AtEnd())
        {
            const char8_t character = cursor.Peek();
            if (lexer_scan::IsSpace(character))
            {
                cursor.Advance();
                continue;
            }
            // Comments: `--` line comment, or `--[[` / `--[=[` long (possibly multi-line) comment.
            if (character == u8'-' && cursor.Peek(1) == u8'-')
            {
                const usize begin = cursor.i;
                const i32 column = cursor.column;
                cursor.Advance();
                cursor.Advance(); // consume `--`
                const int level = LongBracketLevel(cursor, u8'[');
                if (level >= 0)
                {
                    for (int n = 0; n < level + 2; ++n) // '[' + level '=' + '['
                    {
                        cursor.Advance();
                    }
                    const u32 state = ScanLongBracket(cursor, out, static_cast<u32>(level),
                                                      CodeTokenKind::Comment, kModeLongComment,
                                                      begin, column);
                    if (state != 0)
                    {
                        return state;
                    }
                    continue;
                }
                Emit(out, begin, line.Size(), column, CodeTokenKind::Comment); // to end of line
                return 0;
            }
            // Long string `[[ ]]` / `[=[ ]=]` (multi-line).
            if (character == u8'[')
            {
                const int level = LongBracketLevel(cursor, u8'[');
                if (level >= 0)
                {
                    const usize begin = cursor.i;
                    const i32 column = cursor.column;
                    for (int n = 0; n < level + 2; ++n)
                    {
                        cursor.Advance();
                    }
                    const u32 state = ScanLongBracket(cursor, out, static_cast<u32>(level),
                                                      CodeTokenKind::String, kModeLongString, begin,
                                                      column);
                    if (state != 0)
                    {
                        return state;
                    }
                    continue;
                }
                // Not a long bracket - fall through to punctuation.
            }
            if (character == u8'"')
            {
                ScanQuoted(cursor, out, u8'"');
                continue;
            }
            if (character == u8'\'')
            {
                ScanQuoted(cursor, out, u8'\'');
                continue;
            }
            if (character == u8'`') // Luau interpolated string (colored whole, single line)
            {
                ScanQuoted(cursor, out, u8'`');
                continue;
            }
            if (lexer_scan::IsDigit(character) ||
                (character == u8'.' && lexer_scan::IsDigit(cursor.Peek(1))))
            {
                ScanNumber(cursor, out);
                continue;
            }
            if (lexer_scan::IsIdentStart(character))
            {
                ScanWord(cursor, out, m_keywords, m_types);
                continue;
            }
            // Single-glyph operator / punctuation.
            {
                const usize begin = cursor.i;
                const i32 column = cursor.column;
                const CodeTokenKind kind = IsPunctuationChar(character) ? CodeTokenKind::Punctuation
                                                                        : CodeTokenKind::Operator;
                cursor.Advance();
                Emit(out, begin, cursor.i, column, kind);
            }
        }
        return 0;
    }
}
