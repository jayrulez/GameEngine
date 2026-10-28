// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :sss_token partition
//
// Token kinds and the Token record produced by the .sss Tokenizer. Ported from
// Sedulous.UI/src/Styling/Parser/Token.bf.

module;
#include "Core/Prelude.h"

export module foundation.ui:sss_token;

import foundation.core; // StringView

using namespace foundation::core;

export namespace foundation::ui
{
    enum class TokenKind
    {
        EndOfInput,           ///< End of input
        Ident,         ///< Identifier (e.g., Button, background, lighten)
        StringLit,     ///< String literal ("path/to/file")
        Number,        ///< Numeric literal (13, 0.5, 8px, 12dp)
        HexColor,      ///< Hex color (#rrggbb, #rrggbbaa)
        Variable,      ///< Variable reference ($name)
        Directive,     ///< Directive (@palette, @icon, @import, @image)
        PseudoState,   ///< Pseudo-state (:hover, :checked)
        ClassSelector, ///< Class selector (.primary)
        LBrace,        ///< {
        RBrace,        ///< }
        LParen,        ///< (
        RParen,        ///< )
        Colon,         ///< :
        Semicolon,     ///< ;
        Comma,         ///< ,
        Equals,        ///< =
        Percent,       ///< %
        Greater,       ///< > (child combinator)
        Plus,          ///< + (calc operator)
        Minus,         ///< - (calc operator; a `-` that starts a number is part of the Number)
        BoolLit,       ///< Keyword: true/false
        Extends,       ///< Keyword: extends
    };

    struct Token
    {
        TokenKind Kind = TokenKind::EndOfInput;
        StringView Text{};
        i32 Line = 0;
        i32 Column = 0;

        /// For Number tokens: the parsed numeric value.
        f32 NumericValue = 0.0f;

        /// For Number tokens: the unit suffix (px, dp, pt, or empty).
        StringView UnitSuffix{};

        Token() = default;
        Token(TokenKind kind, StringView text, i32 line, i32 col)
            : Kind(kind), Text(text), Line(line), Column(col)
        {
        }
    };
}
