// Ported from Sedulous.Xml.Tests/LexerTests.bf
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.xml;
using namespace draconic::foundation;
using namespace draconic::xml;

TEST_CASE("xml.lexer: whitespace length")
{
    CHECK(XmlLexer::GetWhitespaceLength(u8"") == 0u);
    CHECK(XmlLexer::GetWhitespaceLength(u8"abc") == 0u);
    CHECK(XmlLexer::GetWhitespaceLength(u8" abc") == 1u);
    CHECK(XmlLexer::GetWhitespaceLength(u8"  abc") == 2u);
    CHECK(XmlLexer::GetWhitespaceLength(u8"\t\nabc") == 2u);
    CHECK(XmlLexer::GetWhitespaceLength(u8" \t\r\n abc") == 5u);
    CHECK(XmlLexer::GetWhitespaceLength(u8"   ") == 3u);
}

TEST_CASE("xml.lexer: IsWhitespace")
{
    CHECK(XmlLexer::IsWhitespace(u8' '));
    CHECK(XmlLexer::IsWhitespace(u8'\t'));
    CHECK(XmlLexer::IsWhitespace(u8'\r'));
    CHECK(XmlLexer::IsWhitespace(u8'\n'));
    CHECK_FALSE(XmlLexer::IsWhitespace(u8'a'));
    CHECK_FALSE(XmlLexer::IsWhitespace(u8'0'));
}

TEST_CASE("xml.lexer: ReadName")
{
    usize len = 0;
    CHECK(XmlLexer::ReadName(u8"element", len) == XmlResult::Ok);
    CHECK(len == 7u);
    CHECK(XmlLexer::ReadName(u8"ns:element", len) == XmlResult::Ok);
    CHECK(len == 10u);
    CHECK(XmlLexer::ReadName(u8"_underscore", len) == XmlResult::Ok);
    CHECK(len == 11u);
    CHECK(XmlLexer::ReadName(u8"name123", len) == XmlResult::Ok);
    CHECK(len == 7u);
    CHECK(XmlLexer::ReadName(u8"name-with-dash", len) == XmlResult::Ok);
    CHECK(len == 14u);
    CHECK(XmlLexer::ReadName(u8"name.with.dot", len) == XmlResult::Ok);
    CHECK(len == 13u);
}

TEST_CASE("xml.lexer: ReadName invalid")
{
    usize len = 0;
    CHECK(XmlLexer::ReadName(u8"", len) == XmlResult::NameEmpty);
    CHECK(XmlLexer::ReadName(u8"123invalid", len) == XmlResult::NameEmpty);
    CHECK(XmlLexer::ReadName(u8"-invalid", len) == XmlResult::NameEmpty);
    CHECK(XmlLexer::ReadName(u8".invalid", len) == XmlResult::NameEmpty);
}

TEST_CASE("xml.lexer: ReadName with output")
{
    String output;
    usize len = 0;
    CHECK(XmlLexer::ReadName(u8"element>", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"element"));
    CHECK(len == 7u);
    output.Clear();
    CHECK(XmlLexer::ReadName(u8"ns:tag ", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"ns:tag"));
    CHECK(len == 6u);
}

TEST_CASE("xml.lexer: ReadAttributeValue double/single quotes")
{
    String output;
    usize len = 0;
    CHECK(XmlLexer::ReadAttributeValue(u8"\"hello\"", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"hello"));
    CHECK(len == 7u);
    output.Clear();
    CHECK(XmlLexer::ReadAttributeValue(u8"\"hello world\"", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"hello world"));
    output.Clear();
    CHECK(XmlLexer::ReadAttributeValue(u8"'hello'", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"hello"));
    CHECK(len == 7u);
}

TEST_CASE("xml.lexer: ReadAttributeValue entities + char refs")
{
    String output;
    usize len = 0;
    CHECK(XmlLexer::ReadAttributeValue(u8"\"&amp;\"", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"&"));
    output.Clear();
    CHECK(XmlLexer::ReadAttributeValue(u8"\"&lt;&gt;\"", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"<>"));
    output.Clear();
    CHECK(XmlLexer::ReadAttributeValue(u8"\"&apos;&quot;\"", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"'\""));
    output.Clear();
    CHECK(XmlLexer::ReadAttributeValue(u8"\"&#65;\"", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"A"));
    output.Clear();
    CHECK(XmlLexer::ReadAttributeValue(u8"\"&#x41;\"", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"A"));
    output.Clear();
    CHECK(XmlLexer::ReadAttributeValue(u8"\"&#x3C;\"", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"<"));
}

TEST_CASE("xml.lexer: ReadTextContent")
{
    String output;
    usize len = 0;
    CHECK(XmlLexer::ReadTextContent(u8"hello<", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"hello"));
    CHECK(len == 5u);
    output.Clear();
    CHECK(XmlLexer::ReadTextContent(u8"hello &amp; world<", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"hello & world"));
}

TEST_CASE("xml.lexer: ReadCDataContent")
{
    String output;
    usize len = 0;
    CHECK(XmlLexer::ReadCDataContent(u8"content]]>", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"content"));
    CHECK(len == 10u);
    output.Clear();
    CHECK(XmlLexer::ReadCDataContent(u8"text with <special> chars]]>", len, output) ==
          XmlResult::Ok);
    CHECK(output == StringView(u8"text with <special> chars"));
    output.Clear();
    CHECK(XmlLexer::ReadCDataContent(u8"unclosed content", len, output) ==
          XmlResult::CDataUnclosed);
}

TEST_CASE("xml.lexer: ReadCommentContent")
{
    String output;
    usize len = 0;
    CHECK(XmlLexer::ReadCommentContent(u8"comment-->", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8"comment"));
    CHECK(len == 10u);
    output.Clear();
    CHECK(XmlLexer::ReadCommentContent(u8" this is a comment -->", len, output) == XmlResult::Ok);
    CHECK(output == StringView(u8" this is a comment "));
    output.Clear();
    CHECK(XmlLexer::ReadCommentContent(u8"illegal -- sequence-->", len, output) ==
          XmlResult::CommentIllegalSequence);
}

TEST_CASE("xml.lexer: ReadProcessingInstruction")
{
    String target, data;
    usize len = 0;
    CHECK(XmlLexer::ReadProcessingInstruction(u8"target?>", len, target, data) == XmlResult::Ok);
    CHECK(target == StringView(u8"target"));
    CHECK(data.IsEmpty());
    target.Clear();
    data.Clear();
    CHECK(XmlLexer::ReadProcessingInstruction(u8"target data?>", len, target, data) ==
          XmlResult::Ok);
    CHECK(target == StringView(u8"target"));
    CHECK(data == StringView(u8"data"));
    target.Clear();
    data.Clear();
    CHECK(XmlLexer::ReadProcessingInstruction(u8"php echo 'hello'; ?>", len, target, data) ==
          XmlResult::Ok);
    CHECK(target == StringView(u8"php"));
    CHECK(data == StringView(u8"echo 'hello'; "));
}

TEST_CASE("xml.lexer: IsValidName")
{
    CHECK(XmlLexer::IsValidName(u8"valid"));
    CHECK(XmlLexer::IsValidName(u8"_valid"));
    CHECK(XmlLexer::IsValidName(u8"valid123"));
    CHECK(XmlLexer::IsValidName(u8"ns:name"));
    CHECK(XmlLexer::IsValidName(u8"name-with-dash"));
    CHECK_FALSE(XmlLexer::IsValidName(u8""));
    CHECK_FALSE(XmlLexer::IsValidName(u8"123invalid"));
    CHECK_FALSE(XmlLexer::IsValidName(u8"-invalid"));
}

TEST_CASE("xml.lexer: SplitQualifiedName")
{
    String prefix, localName;
    XmlLexer::SplitQualifiedName(u8"element", prefix, localName);
    CHECK(prefix.IsEmpty());
    CHECK(localName == StringView(u8"element"));
    prefix.Clear();
    localName.Clear();
    XmlLexer::SplitQualifiedName(u8"ns:element", prefix, localName);
    CHECK(prefix == StringView(u8"ns"));
    CHECK(localName == StringView(u8"element"));
    prefix.Clear();
    localName.Clear();
    XmlLexer::SplitQualifiedName(u8"a:b:c", prefix, localName);
    CHECK(prefix == StringView(u8"a"));
    CHECK(localName == StringView(u8"b:c"));
}

TEST_CASE("xml.lexer: reference errors")
{
    String output;
    usize len = 0;
    CHECK(XmlLexer::DecodeReference(u8"&unknown;", len, output) == XmlResult::EntityUnknown);
    output.Clear();
    CHECK(XmlLexer::DecodeReference(u8"&#x110000;", len, output) == XmlResult::CharRefOutOfRange);
    output.Clear();
    CHECK(XmlLexer::DecodeReference(u8"&#0;", len, output) == XmlResult::CharRefOutOfRange);
}
