// Ported from Sedulous.UI.Tests/src/TokenizerTests.bf (faithful; Beef `scope Tokenizer` -> stack
// value, List<Token> -> Array<Token>, Math.Abs(...) < eps -> doctest::Approx).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;

using namespace draconic::ui;
namespace foundation = draconic::foundation;
using namespace draconic::foundation;

TEST_CASE("tokenizer: Ident")
{
    Tokenizer tok(u8"Button");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::Ident);
    CHECK(t.Text == StringView(u8"Button"));
}

TEST_CASE("tokenizer: HyphenatedIdent")
{
    Tokenizer tok(u8"text-color");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::Ident);
    CHECK(t.Text == StringView(u8"text-color"));
}

TEST_CASE("tokenizer: HexColor_6Digit")
{
    Tokenizer tok(u8"#4a8eff");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::HexColor);
    CHECK(t.Text == StringView(u8"#4a8eff"));
}

TEST_CASE("tokenizer: HexColor_8Digit")
{
    Tokenizer tok(u8"#4a8effcc");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::HexColor);
    CHECK(t.Text == StringView(u8"#4a8effcc"));
}

TEST_CASE("tokenizer: Variable")
{
    Tokenizer tok(u8"$surface-bright");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::Variable);
    CHECK(t.Text == StringView(u8"$surface-bright"));
}

TEST_CASE("tokenizer: Directive")
{
    Tokenizer tok(u8"@palette");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::Directive);
    CHECK(t.Text == StringView(u8"@palette"));
}

TEST_CASE("tokenizer: PseudoState")
{
    Tokenizer tok(u8":hover");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::PseudoState);
    CHECK(t.Text == StringView(u8":hover"));
}

TEST_CASE("tokenizer: PseudoState_Hyphenated")
{
    Tokenizer tok(u8":focus-within");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::PseudoState);
    CHECK(t.Text == StringView(u8":focus-within"));
}

TEST_CASE("tokenizer: ClassSelector")
{
    Tokenizer tok(u8".primary");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::ClassSelector);
    CHECK(t.Text == StringView(u8".primary"));
}

TEST_CASE("tokenizer: ClassSelector_Hyphenated")
{
    Tokenizer tok(u8".label-dim");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::ClassSelector);
    CHECK(t.Text == StringView(u8".label-dim"));
}

TEST_CASE("tokenizer: Number_Integer")
{
    Tokenizer tok(u8"42");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::Number);
    CHECK(t.NumericValue == 42);
}

TEST_CASE("tokenizer: Number_Float")
{
    Tokenizer tok(u8"3.14");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::Number);
    CHECK(t.NumericValue == doctest::Approx(3.14f).epsilon(0.01));
}

TEST_CASE("tokenizer: Number_WithUnit")
{
    Tokenizer tok(u8"16px");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::Number);
    CHECK(t.NumericValue == 16);
    CHECK(t.UnitSuffix == StringView(u8"px"));
}

TEST_CASE("tokenizer: Number_Negative")
{
    Tokenizer tok(u8"-5");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::Number);
    CHECK(t.NumericValue == -5);
}

TEST_CASE("tokenizer: StringLiteral")
{
    Tokenizer tok(u8"\"icons/check.svg\"");
    const Token t = tok.NextToken();
    CHECK(t.Kind == TokenKind::StringLit);
    CHECK(t.Text == StringView(u8"icons/check.svg"));
}

TEST_CASE("tokenizer: BoolLiterals")
{
    Tokenizer tok(u8"true false");
    const Token t1 = tok.NextToken();
    CHECK((t1.Kind == TokenKind::BoolLit && t1.Text == StringView(u8"true")));
    const Token t2 = tok.NextToken();
    CHECK((t2.Kind == TokenKind::BoolLit && t2.Text == StringView(u8"false")));
}

TEST_CASE("tokenizer: Extends")
{
    Tokenizer tok(u8"extends");
    CHECK(tok.NextToken().Kind == TokenKind::Extends);
}

TEST_CASE("tokenizer: Punctuation")
{
    Tokenizer tok(u8"{ } ( ) : ; , = %");
    CHECK(tok.NextToken().Kind == TokenKind::LBrace);
    CHECK(tok.NextToken().Kind == TokenKind::RBrace);
    CHECK(tok.NextToken().Kind == TokenKind::LParen);
    CHECK(tok.NextToken().Kind == TokenKind::RParen);
    CHECK(tok.NextToken().Kind == TokenKind::Colon);
    CHECK(tok.NextToken().Kind == TokenKind::Semicolon);
    CHECK(tok.NextToken().Kind == TokenKind::Comma);
    CHECK(tok.NextToken().Kind == TokenKind::Equals);
    CHECK(tok.NextToken().Kind == TokenKind::Percent);
}

TEST_CASE("tokenizer: Comment_Skipped")
{
    Tokenizer tok(u8"/* comment */ Button");
    const Token t = tok.NextToken();
    CHECK((t.Kind == TokenKind::Ident && t.Text == StringView(u8"Button")));
}

TEST_CASE("tokenizer: MultilineComment")
{
    Tokenizer tok(u8"/* line1\nline2 */ View");
    const Token t = tok.NextToken();
    CHECK((t.Kind == TokenKind::Ident && t.Text == StringView(u8"View")));
}

TEST_CASE("tokenizer: EOF")
{
    Tokenizer tok(u8"");
    CHECK(tok.NextToken().Kind == TokenKind::EndOfInput);
}

TEST_CASE("tokenizer: FullSelector_Tokenizes")
{
    Tokenizer tok(u8"Button.primary:hover");
    foundation::Array<Token> tokens;
    tok.TokenizeAll(tokens);

    CHECK(tokens.Size() >= 4); // Ident, ClassSelector, PseudoState, EOF
    CHECK((tokens[0].Kind == TokenKind::Ident && tokens[0].Text == StringView(u8"Button")));
    CHECK(
        (tokens[1].Kind == TokenKind::ClassSelector && tokens[1].Text == StringView(u8".primary")));
    CHECK((tokens[2].Kind == TokenKind::PseudoState && tokens[2].Text == StringView(u8":hover")));
}

TEST_CASE("tokenizer: CompoundState_Tokenizes")
{
    Tokenizer tok(u8"CheckBox:checked:hover");
    foundation::Array<Token> tokens;
    tok.TokenizeAll(tokens);

    CHECK((tokens[0].Kind == TokenKind::Ident && tokens[0].Text == StringView(u8"CheckBox")));
    CHECK((tokens[1].Kind == TokenKind::PseudoState && tokens[1].Text == StringView(u8":checked")));
    CHECK((tokens[2].Kind == TokenKind::PseudoState && tokens[2].Text == StringView(u8":hover")));
}

TEST_CASE("tokenizer: LineTracking")
{
    Tokenizer tok(u8"a\nb");
    const Token t1 = tok.NextToken();
    CHECK(t1.Line == 1);
    const Token t2 = tok.NextToken();
    CHECK(t2.Line == 2);
}

TEST_CASE("tokenizer: DirectiveVariants")
{
    Tokenizer tok(u8"@palette @icon @image @import");
    CHECK(tok.NextToken().Text == StringView(u8"@palette"));
    CHECK(tok.NextToken().Text == StringView(u8"@icon"));
    CHECK(tok.NextToken().Text == StringView(u8"@image"));
    CHECK(tok.NextToken().Text == StringView(u8"@import"));
}
