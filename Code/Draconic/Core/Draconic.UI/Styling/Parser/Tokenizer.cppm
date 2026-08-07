// Draconic UI - :sss_tokenizer partition
//
// Lexer for .sss stylesheet files. Ported from Sedulous.UI/src/Styling/Parser/Tokenizer.bf.
//
// Divergences (language): Beef char8 classifier methods (.IsDigit/.IsLetter/.IsLetterOrDigit) become
// local ASCII helpers over utf8char; StringView.Substring -> SubStr; float.Parse -> StrToF32.

module;
#include "Draconic.Foundation/Prelude.h"
#include <cstdlib> // std::strtof

export module draconic.ui:sss_tokenizer;

import draconic.foundation; // StringView, Array
import :sss_token;

using namespace draconic::foundation;

namespace draconic::ui::detail
{
    [[nodiscard]] inline bool IsDigitC(utf8char c) { return c >= u8'0' && c <= u8'9'; }
    [[nodiscard]] inline bool IsLetterC(utf8char c)
    {
        return (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z');
    }
    [[nodiscard]] inline bool IsLetterOrDigitC(utf8char c) { return IsLetterC(c) || IsDigitC(c); }
    [[nodiscard]] inline bool IsHexDigitC(utf8char c)
    {
        return IsDigitC(c) || (c >= u8'a' && c <= u8'f') || (c >= u8'A' && c <= u8'F');
    }
    [[nodiscard]] inline bool IsIdentStartC(utf8char c) { return IsLetterC(c) || c == u8'_'; }
    [[nodiscard]] inline bool IsIdentCharC(utf8char c) { return IsLetterOrDigitC(c) || c == u8'_'; }

    // Parse an already-scanned numeric substring to f32 via strtof (empty/invalid -> 0).
    [[nodiscard]] inline f32 StrToF32(StringView s)
    {
        if (s.Size() == 0)
        {
            return 0.0f;
        }
        char buf[64];
        const usize n = s.Size() < 63 ? s.Size() : 63;
        for (usize i = 0; i < n; ++i)
        {
            buf[i] = static_cast<char>(s[i]);
        }
        buf[n] = '\0';
        char* end = nullptr;
        const f32 v = std::strtof(buf, &end);
        return (end == buf) ? 0.0f : v;
    }
}

export namespace draconic::ui
{
    /// Lexer for .sss stylesheet files.
    class Tokenizer
    {
    public:
        explicit Tokenizer(StringView source) : m_source(source) {}

        Token NextToken()
        {
            SkipWhitespaceAndComments();

            if (m_pos >= static_cast<i32>(m_source.Size()))
                return Token(TokenKind::EndOfInput, u8"", m_line, m_col);

            const i32 startLine = m_line;
            const i32 startCol = m_col;
            const utf8char ch = m_source[static_cast<usize>(m_pos)];

            // Single-character tokens
            switch (ch)
            {
            case u8'{':
                Advance();
                return Token(TokenKind::LBrace, u8"{", startLine, startCol);
            case u8'}':
                Advance();
                return Token(TokenKind::RBrace, u8"}", startLine, startCol);
            case u8'(':
                Advance();
                return Token(TokenKind::LParen, u8"(", startLine, startCol);
            case u8')':
                Advance();
                return Token(TokenKind::RParen, u8")", startLine, startCol);
            case u8';':
                Advance();
                return Token(TokenKind::Semicolon, u8";", startLine, startCol);
            case u8',':
                Advance();
                return Token(TokenKind::Comma, u8",", startLine, startCol);
            case u8'=':
                Advance();
                return Token(TokenKind::Equals, u8"=", startLine, startCol);
            case u8'%':
                Advance();
                return Token(TokenKind::Percent, u8"%", startLine, startCol);
            default:
                break;
            }

            // Hex color: #rrggbb or #rrggbbaa
            if (ch == u8'#')
                return ReadHexColor(startLine, startCol);

            // Variable: $name
            if (ch == u8'$')
                return ReadVariable(startLine, startCol);

            // Directive: @palette, @icon, @import, @image
            if (ch == u8'@')
                return ReadDirective(startLine, startCol);

            // Pseudo-state: :hover, :checked, etc. But not standalone : (property separator).
            if (ch == u8':')
            {
                if (m_pos + 1 < static_cast<i32>(m_source.Size()) &&
                    detail::IsIdentStartC(m_source[static_cast<usize>(m_pos + 1)]))
                    return ReadPseudoState(startLine, startCol);
                Advance();
                return Token(TokenKind::Colon, u8":", startLine, startCol);
            }

            // Class selector: .primary
            if (ch == u8'.')
            {
                if (m_pos + 1 < static_cast<i32>(m_source.Size()) &&
                    detail::IsIdentStartC(m_source[static_cast<usize>(m_pos + 1)]))
                    return ReadClassSelector(startLine, startCol);
                // Might be start of a number like .5
                if (m_pos + 1 < static_cast<i32>(m_source.Size()) &&
                    detail::IsDigitC(m_source[static_cast<usize>(m_pos + 1)]))
                    return ReadNumber(startLine, startCol);
                Advance();
                return Token(TokenKind::Colon, u8".", startLine, startCol);
            }

            // String literal: "..."
            if (ch == u8'"')
                return ReadString(startLine, startCol);

            // Number: starts with digit or negative sign
            if (detail::IsDigitC(ch) ||
                (ch == u8'-' && m_pos + 1 < static_cast<i32>(m_source.Size()) &&
                 (detail::IsDigitC(m_source[static_cast<usize>(m_pos + 1)]) ||
                  m_source[static_cast<usize>(m_pos + 1)] == u8'.')))
                return ReadNumber(startLine, startCol);

            // Identifier or keyword
            if (detail::IsIdentStartC(ch))
                return ReadIdent(startLine, startCol);

            // Unknown - skip
            Advance();
            return Token(TokenKind::EndOfInput, u8"", startLine, startCol);
        }

        /// Tokenize the entire source into a list.
        void TokenizeAll(Array<Token>& tokens)
        {
            while (true)
            {
                const Token tok = NextToken();
                tokens.PushBack(tok);
                if (tok.Kind == TokenKind::EndOfInput)
                    break;
            }
        }

    private:
        // === Readers ===

        Token ReadHexColor(i32 line, i32 col)
        {
            const i32 start = m_pos;
            Advance(); // skip #
            while (m_pos < static_cast<i32>(m_source.Size()) &&
                   detail::IsHexDigitC(m_source[static_cast<usize>(m_pos)]))
                Advance();
            return Token(TokenKind::HexColor, Slice(start, m_pos), line, col);
        }

        Token ReadVariable(i32 line, i32 col)
        {
            const i32 start = m_pos;
            Advance(); // skip $
            while (m_pos < static_cast<i32>(m_source.Size()) &&
                   (detail::IsIdentCharC(m_source[static_cast<usize>(m_pos)]) ||
                    m_source[static_cast<usize>(m_pos)] == u8'-'))
                Advance();
            return Token(TokenKind::Variable, Slice(start, m_pos), line, col);
        }

        Token ReadDirective(i32 line, i32 col)
        {
            const i32 start = m_pos;
            Advance(); // skip @
            while (m_pos < static_cast<i32>(m_source.Size()) &&
                   detail::IsIdentCharC(m_source[static_cast<usize>(m_pos)]))
                Advance();
            return Token(TokenKind::Directive, Slice(start, m_pos), line, col);
        }

        Token ReadPseudoState(i32 line, i32 col)
        {
            const i32 start = m_pos;
            Advance(); // skip :
            while (m_pos < static_cast<i32>(m_source.Size()) &&
                   (detail::IsIdentCharC(m_source[static_cast<usize>(m_pos)]) ||
                    m_source[static_cast<usize>(m_pos)] == u8'-'))
                Advance();
            return Token(TokenKind::PseudoState, Slice(start, m_pos), line, col);
        }

        Token ReadClassSelector(i32 line, i32 col)
        {
            const i32 start = m_pos;
            Advance(); // skip .
            while (m_pos < static_cast<i32>(m_source.Size()) &&
                   (detail::IsIdentCharC(m_source[static_cast<usize>(m_pos)]) ||
                    m_source[static_cast<usize>(m_pos)] == u8'-'))
                Advance();
            return Token(TokenKind::ClassSelector, Slice(start, m_pos), line, col);
        }

        Token ReadString(i32 line, i32 col)
        {
            Advance(); // skip opening "
            const i32 start = m_pos;
            while (m_pos < static_cast<i32>(m_source.Size()) &&
                   m_source[static_cast<usize>(m_pos)] != u8'"')
            {
                if (m_source[static_cast<usize>(m_pos)] == u8'\n')
                {
                    m_line++;
                    m_col = 0;
                }
                Advance();
            }
            const StringView text = Slice(start, m_pos);
            if (m_pos < static_cast<i32>(m_source.Size()))
                Advance(); // skip closing "
            return Token(TokenKind::StringLit, text, line, col);
        }

        Token ReadNumber(i32 line, i32 col)
        {
            const i32 start = m_pos;
            if (m_pos < static_cast<i32>(m_source.Size()) &&
                m_source[static_cast<usize>(m_pos)] == u8'-')
                Advance();
            while (m_pos < static_cast<i32>(m_source.Size()) &&
                   detail::IsDigitC(m_source[static_cast<usize>(m_pos)]))
                Advance();
            if (m_pos < static_cast<i32>(m_source.Size()) &&
                m_source[static_cast<usize>(m_pos)] == u8'.')
            {
                Advance();
                while (m_pos < static_cast<i32>(m_source.Size()) &&
                       detail::IsDigitC(m_source[static_cast<usize>(m_pos)]))
                    Advance();
            }

            const StringView numText = Slice(start, m_pos);

            // Check for unit suffix (px, dp, pt)
            const i32 unitStart = m_pos;
            while (m_pos < static_cast<i32>(m_source.Size()) &&
                   detail::IsLetterC(m_source[static_cast<usize>(m_pos)]))
                Advance();
            const StringView unitText = Slice(unitStart, m_pos);
            const StringView fullText = Slice(start, m_pos);

            Token tok(TokenKind::Number, fullText, line, col);
            tok.NumericValue = detail::StrToF32(numText);
            tok.UnitSuffix = unitText;
            return tok;
        }

        Token ReadIdent(i32 line, i32 col)
        {
            const i32 start = m_pos;
            while (m_pos < static_cast<i32>(m_source.Size()) &&
                   (detail::IsIdentCharC(m_source[static_cast<usize>(m_pos)]) ||
                    m_source[static_cast<usize>(m_pos)] == u8'-'))
                Advance();
            const StringView text = Slice(start, m_pos);

            // Keywords
            if (text == StringView(u8"true"))
                return Token(TokenKind::BoolLit, text, line, col);
            if (text == StringView(u8"false"))
                return Token(TokenKind::BoolLit, text, line, col);
            if (text == StringView(u8"extends"))
                return Token(TokenKind::Extends, text, line, col);

            return Token(TokenKind::Ident, text, line, col);
        }

        // === Helpers ===

        void SkipWhitespaceAndComments()
        {
            while (m_pos < static_cast<i32>(m_source.Size()))
            {
                const utf8char ch = m_source[static_cast<usize>(m_pos)];

                if (ch == u8' ' || ch == u8'\t' || ch == u8'\r')
                {
                    Advance();
                    continue;
                }

                if (ch == u8'\n')
                {
                    m_pos++;
                    m_line++;
                    m_col = 1;
                    continue;
                }

                // Block comment /* ... */
                if (ch == u8'/' && m_pos + 1 < static_cast<i32>(m_source.Size()) &&
                    m_source[static_cast<usize>(m_pos + 1)] == u8'*')
                {
                    m_pos += 2;
                    m_col += 2;
                    while (m_pos + 1 < static_cast<i32>(m_source.Size()))
                    {
                        if (m_source[static_cast<usize>(m_pos)] == u8'\n')
                        {
                            m_line++;
                            m_col = 0;
                        }
                        if (m_source[static_cast<usize>(m_pos)] == u8'*' &&
                            m_source[static_cast<usize>(m_pos + 1)] == u8'/')
                        {
                            m_pos += 2;
                            m_col += 2;
                            break;
                        }
                        m_pos++;
                        m_col++;
                    }
                    continue;
                }

                break;
            }
        }

        void Advance()
        {
            m_pos++;
            m_col++;
        }

        /// Substring [start, end) of the source.
        [[nodiscard]] StringView Slice(i32 start, i32 end) const
        {
            return m_source.SubStr(static_cast<usize>(start), static_cast<usize>(end - start));
        }

        StringView m_source;
        i32 m_pos = 0;
        i32 m_line = 1;
        i32 m_col = 1;
    };
}
