// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - XmlLexer implementation (declared in :code_lexer).
//
// Stateful markup lexer: tags, attributes, quoted values, comments, CDATA, and <?...?>
// declarations, all spanning lines via the mode carried in the line state. Text content
// between tags emits Default-kind runs (the renderer draws only tokens).

module;
#include "Core/Prelude.h"

module foundation.ui.toolkit;

import foundation.core;
import :code_lexer_scan;

using namespace foundation::core;

namespace foundation::ui::toolkit
{
    u32 XmlLexer::LexLine(StringView line, u32 entryState, Array<CodeToken>& out)
    {
        using lexer_scan::Cursor;
        using lexer_scan::Emit;

        Cursor cursor{line};
        u32 mode = entryState;
        usize runBegin = 0;
        i32 runColumn = 0;

        const auto beginRun = [&]
        {
            runBegin = cursor.i;
            runColumn = cursor.column;
        };

        beginRun();
        while (!cursor.AtEnd())
        {
            switch (mode)
            {
                case kModeText:
                    if (cursor.Peek() == u8'<')
                    {
                        if (cursor.Match(u8"<!--", 4))
                        {
                            beginRun();
                            cursor.Advance();
                            cursor.Advance();
                            cursor.Advance();
                            cursor.Advance();
                            mode = kModeComment;
                            break;
                        }
                        if (cursor.Match(u8"<![CDATA[", 9))
                        {
                            beginRun();
                            for (i32 k = 0; k < 9; ++k)
                            {
                                cursor.Advance();
                            }
                            mode = kModeCData;
                            break;
                        }
                        if (cursor.Match(u8"<?", 2))
                        {
                            beginRun();
                            cursor.Advance();
                            cursor.Advance();
                            mode = kModeDeclaration;
                            break;
                        }
                        // "<" or "</" punctuation, then the element name as Tag.
                        beginRun();
                        cursor.Advance();
                        if (cursor.Peek() == u8'/')
                        {
                            cursor.Advance();
                        }
                        Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::Punctuation);
                        beginRun();
                        while (!cursor.AtEnd() && (lexer_scan::IsIdentChar(cursor.Peek()) ||
                                              cursor.Peek() == u8':' || cursor.Peek() == u8'-'))
                        {
                            cursor.Advance();
                        }
                        Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::Tag);
                        mode = kModeInTag;
                        break;
                    }
                    // Plain text content: one Default-kind run up to the next '<' (the
                    // renderer draws ONLY tokens, so text must be covered by one).
                    beginRun();
                    while (!cursor.AtEnd() && cursor.Peek() != u8'<')
                    {
                        cursor.Advance();
                    }
                    Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::Default);
                    break;

                case kModeInTag:
                    if (lexer_scan::IsSpace(cursor.Peek()))
                    {
                        cursor.Advance();
                        break;
                    }
                    if (cursor.Peek() == u8'>')
                    {
                        beginRun();
                        cursor.Advance();
                        Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::Punctuation);
                        mode = kModeText;
                        break;
                    }
                    if (cursor.Match(u8"/>", 2))
                    {
                        beginRun();
                        cursor.Advance();
                        cursor.Advance();
                        Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::Punctuation);
                        mode = kModeText;
                        break;
                    }
                    if (cursor.Peek() == u8'=')
                    {
                        beginRun();
                        cursor.Advance();
                        Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::Operator);
                        break;
                    }
                    if (cursor.Peek() == u8'"' || cursor.Peek() == u8'\'')
                    {
                        beginRun();
                        const char8_t quote = cursor.Peek();
                        cursor.Advance();
                        mode = quote == u8'"' ? kModeDoubleQuote : kModeSingleQuote;
                        break;
                    }
                    if (lexer_scan::IsIdentStart(cursor.Peek()))
                    {
                        beginRun();
                        while (!cursor.AtEnd() && (lexer_scan::IsIdentChar(cursor.Peek()) ||
                                              cursor.Peek() == u8':' || cursor.Peek() == u8'-'))
                        {
                            cursor.Advance();
                        }
                        Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::Attribute);
                        break;
                    }
                    beginRun();
                    cursor.Advance();
                    Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::Operator);
                    break;

                case kModeComment:
                    if (cursor.Match(u8"-->", 3))
                    {
                        cursor.Advance();
                        cursor.Advance();
                        cursor.Advance();
                        Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::Comment);
                        mode = kModeText;
                        beginRun();
                        break;
                    }
                    cursor.Advance();
                    break;

                case kModeCData:
                    if (cursor.Match(u8"]]>", 3))
                    {
                        cursor.Advance();
                        cursor.Advance();
                        cursor.Advance();
                        Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::String);
                        mode = kModeText;
                        beginRun();
                        break;
                    }
                    cursor.Advance();
                    break;

                case kModeDeclaration:
                    if (cursor.Match(u8"?>", 2))
                    {
                        cursor.Advance();
                        cursor.Advance();
                        Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::Preprocessor);
                        mode = kModeText;
                        beginRun();
                        break;
                    }
                    cursor.Advance();
                    break;

                case kModeDoubleQuote:
                case kModeSingleQuote:
                {
                    const char8_t quote = mode == kModeDoubleQuote ? u8'"' : u8'\'';
                    if (cursor.Peek() == quote)
                    {
                        cursor.Advance();
                        Emit(out, runBegin, cursor.i, runColumn, CodeTokenKind::String);
                        mode = kModeInTag;
                        beginRun();
                        break;
                    }
                    cursor.Advance();
                    break;
                }

                default:
                    cursor.Advance();
                    break;
            }
        }

        // Flush the open multi-line run (comment/CDATA/declaration/attribute string).
        switch (mode)
        {
            case kModeComment:
                lexer_scan::Emit(out, runBegin, line.Size(), runColumn, CodeTokenKind::Comment);
                break;
            case kModeCData:
            case kModeDoubleQuote:
            case kModeSingleQuote:
                lexer_scan::Emit(out, runBegin, line.Size(), runColumn, CodeTokenKind::String);
                break;
            case kModeDeclaration:
                lexer_scan::Emit(out, runBegin, line.Size(), runColumn, CodeTokenKind::Preprocessor);
                break;
            default:
                break;
        }
        return mode;
    }
}
