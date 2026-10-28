// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - CLikeLexer implementation (declared in :code_lexer).
//
// One configurable scanner covers the C-family languages the editor hosts; language identity
// lives entirely in the CLikeLexerSpec its OWNER supplies (keyword/type tables + quirk flags:
// nested block comments, triple-quoted strings, '#' preprocessor lines, char literals).
// Toolkit ships no language tables - see the layering note in CodeLexer.cppm.

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

        constexpr u32 kModeBlockComment = 1;
        constexpr u32 kModeTripleString = 2;

        /// Scans from inside a block comment (depth >= 1). Returns 0 when it closed on this
        /// line, else the carry state. tokenBegin/Column describe where the token starts
        /// (line start when resuming, the "/*" when opened here).
        u32 ScanBlockComment(Cursor& cursor, Array<CodeToken>& out, u32 depth, bool nested,
                             usize tokenBegin, i32 tokenColumn)
        {
            while (!cursor.AtEnd())
            {
                if (cursor.Match(u8"*/", 2))
                {
                    cursor.Advance();
                    cursor.Advance();
                    if (--depth == 0)
                    {
                        Emit(out, tokenBegin, cursor.i, tokenColumn, CodeTokenKind::Comment);
                        return 0;
                    }
                    continue;
                }
                if (nested && cursor.Match(u8"/*", 2))
                {
                    cursor.Advance();
                    cursor.Advance();
                    ++depth;
                    continue;
                }
                cursor.Advance();
            }
            Emit(out, tokenBegin, cursor.i, tokenColumn, CodeTokenKind::Comment);
            return kModeBlockComment | (depth << 8);
        }

        u32 ScanTripleString(Cursor& cursor, Array<CodeToken>& out, usize tokenBegin, i32 tokenColumn)
        {
            while (!cursor.AtEnd())
            {
                if (cursor.Match(u8"\"\"\"", 3))
                {
                    cursor.Advance();
                    cursor.Advance();
                    cursor.Advance();
                    Emit(out, tokenBegin, cursor.i, tokenColumn, CodeTokenKind::String);
                    return 0;
                }
                cursor.Advance();
            }
            Emit(out, tokenBegin, cursor.i, tokenColumn, CodeTokenKind::String);
            return kModeTripleString;
        }

    }

    CLikeLexer::CLikeLexer(const CLikeLexerSpec& spec) : m_spec(spec)
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

    u32 CLikeLexer::LexLine(StringView line, u32 entryState, Array<CodeToken>& out)
    {
        Cursor cursor{line};

        // Resume a multi-line construct.
        const u32 mode = entryState & 0xFFu;
        if (mode == kModeBlockComment)
        {
            const u32 state = ScanBlockComment(cursor, out, entryState >> 8,
                                               m_spec.nestedBlockComments, 0, 0);
            if (state != 0)
            {
                return state;
            }
        }
        else if (mode == kModeTripleString)
        {
            const u32 state = ScanTripleString(cursor, out, 0, 0);
            if (state != 0)
            {
                return state;
            }
        }

        // '#' as the line's first glyph: the whole line is one preprocessor token.
        if (m_spec.hashPreprocessorLines)
        {
            Cursor probe = cursor;
            while (!probe.AtEnd() && lexer_scan::IsSpace(probe.Peek()))
            {
                probe.Advance();
            }
            if (!probe.AtEnd() && probe.Peek() == u8'#')
            {
                Emit(out, probe.i, line.Size(), probe.column, CodeTokenKind::Preprocessor);
                return 0;
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
            if (character == u8'/' && cursor.Peek(1) == u8'/')
            {
                Emit(out, cursor.i, line.Size(), cursor.column, CodeTokenKind::Comment);
                return 0;
            }
            if (character == u8'/' && cursor.Peek(1) == u8'*')
            {
                const usize begin = cursor.i;
                const i32 column = cursor.column;
                cursor.Advance();
                cursor.Advance();
                const u32 state =
                    ScanBlockComment(cursor, out, 1, m_spec.nestedBlockComments, begin, column);
                if (state != 0)
                {
                    return state;
                }
                continue;
            }
            if (character == u8'"')
            {
                if (m_spec.tripleQuotedStrings && cursor.Match(u8"\"\"\"", 3))
                {
                    const usize begin = cursor.i;
                    const i32 column = cursor.column;
                    cursor.Advance();
                    cursor.Advance();
                    cursor.Advance();
                    const u32 state = ScanTripleString(cursor, out, begin, column);
                    if (state != 0)
                    {
                        return state;
                    }
                    continue;
                }
                ScanQuoted(cursor, out, u8'"');
                continue;
            }
            if (m_spec.charLiterals && character == u8'\'')
            {
                ScanQuoted(cursor, out, u8'\'');
                continue;
            }
            if (lexer_scan::IsDigit(character) || (character == u8'.' && lexer_scan::IsDigit(cursor.Peek(1))))
            {
                ScanNumber(cursor, out);
                continue;
            }
            if (lexer_scan::IsIdentStart(character))
            {
                ScanWord(cursor, out, m_keywords, m_types);
                continue;
            }
            // Single-glyph operator/punctuation.
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
