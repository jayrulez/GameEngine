// Ported from Sedulous.Xml.Tests/WriterTests.bf
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.xml;
using namespace draconic::foundation;
using namespace draconic::xml;

namespace
{
    bool Contains(StringView haystack, StringView needle)
    {
        if (needle.Size() > haystack.Size())
        {
            return false;
        }
        for (usize i = 0; i + needle.Size() <= haystack.Size(); ++i)
        {
            bool match = true;
            for (usize j = 0; j < needle.Size(); ++j)
            {
                if (haystack[i + j] != needle[j])
                {
                    match = false;
                    break;
                }
            }
            if (match)
            {
                return true;
            }
        }
        return false;
    }
    XmlWriteSettings NoDecl()
    {
        XmlWriteSettings s = XmlWriteSettings::Default();
        s.OmitDeclaration = true;
        return s;
    }
}

TEST_CASE("xml.write: simple element")
{
    XmlDocument doc;
    doc.AppendChild(doc.CreateElement(u8"root"));
    String output;
    doc.WriteTo(output);
    CHECK(Contains(output, u8"<root/>"));
}

TEST_CASE("xml.write: element with content")
{
    XmlDocument doc;
    XmlElement* root = doc.CreateElement(u8"root");
    root->AppendChild(doc.CreateTextNode(u8"Hello"));
    doc.AppendChild(root);
    String output;
    doc.WriteTo(output, NoDecl());
    CHECK(Contains(output, u8"<root>Hello</root>"));
}

TEST_CASE("xml.write: nested elements (compact)")
{
    XmlDocument doc;
    XmlElement* root = doc.CreateElement(u8"root");
    XmlElement* child = doc.CreateElement(u8"child");
    child->AppendChild(doc.CreateElement(u8"grandchild"));
    root->AppendChild(child);
    doc.AppendChild(root);
    XmlWriteSettings s = NoDecl();
    s.CompactMode = true;
    String output;
    doc.WriteTo(output, s);
    CHECK(output == StringView(u8"<root><child><grandchild/></child></root>"));
}

TEST_CASE("xml.write: attributes")
{
    XmlDocument doc;
    XmlElement* root = doc.CreateElement(u8"root");
    root->SetAttribute(u8"id", u8"123");
    root->SetAttribute(u8"name", u8"test");
    doc.AppendChild(root);
    String output;
    doc.WriteTo(output, NoDecl());
    CHECK(Contains(output, u8"id=\"123\""));
    CHECK(Contains(output, u8"name=\"test\""));
}

TEST_CASE("xml.write: text escaping")
{
    XmlDocument doc;
    XmlElement* root = doc.CreateElement(u8"root");
    root->AppendChild(doc.CreateTextNode(u8"a < b & c > d"));
    doc.AppendChild(root);
    String output;
    doc.WriteTo(output, NoDecl());
    CHECK(Contains(output, u8"a &lt; b &amp; c &gt; d"));
}

TEST_CASE("xml.write: attribute escaping")
{
    XmlDocument doc;
    XmlElement* root = doc.CreateElement(u8"root");
    root->SetAttribute(u8"value", u8"a\"b'c<d>e&f");
    doc.AppendChild(root);
    String output;
    doc.WriteTo(output, NoDecl());
    CHECK(Contains(output, u8"&quot;"));
    CHECK(Contains(output, u8"&apos;"));
    CHECK(Contains(output, u8"&lt;"));
    CHECK(Contains(output, u8"&gt;"));
    CHECK(Contains(output, u8"&amp;"));
}

TEST_CASE("xml.write: compact mode")
{
    XmlDocument doc;
    XmlElement* root = doc.CreateElement(u8"root");
    root->AppendChild(doc.CreateElement(u8"child"));
    doc.AppendChild(root);
    XmlWriteSettings s = NoDecl();
    s.CompactMode = true;
    String output;
    doc.WriteTo(output, s);
    CHECK_FALSE(Contains(output, u8"\n"));
    CHECK_FALSE(Contains(output, u8"\t"));
    CHECK(output == StringView(u8"<root><child/></root>"));
}

TEST_CASE("xml.write: indentation")
{
    XmlDocument doc;
    XmlElement* root = doc.CreateElement(u8"root");
    root->AppendChild(doc.CreateElement(u8"child"));
    doc.AppendChild(root);
    XmlWriteSettings s = NoDecl();
    s.Indent = true;
    s.IndentString = u8"  ";
    String output;
    doc.WriteTo(output, s);
    CHECK(Contains(output, u8"\n"));
    CHECK(Contains(output, u8"  <child/>"));
}

TEST_CASE("xml.write: CDATA + comment + PI")
{
    {
        XmlDocument doc;
        XmlElement* root = doc.CreateElement(u8"root");
        root->AppendChild(doc.CreateCDataSection(u8"<special> & content"));
        doc.AppendChild(root);
        String output;
        doc.WriteTo(output, NoDecl());
        CHECK(Contains(output, u8"<![CDATA[<special> & content]]>"));
    }
    {
        XmlDocument doc;
        XmlElement* root = doc.CreateElement(u8"root");
        root->AppendChild(doc.CreateComment(u8" this is a comment "));
        doc.AppendChild(root);
        String output;
        doc.WriteTo(output, NoDecl());
        CHECK(Contains(output, u8"<!-- this is a comment -->"));
    }
    {
        XmlDocument doc;
        XmlElement* root = doc.CreateElement(u8"root");
        root->AppendChild(doc.CreateProcessingInstruction(u8"php", u8"echo 'hello';"));
        doc.AppendChild(root);
        String output;
        doc.WriteTo(output, NoDecl());
        CHECK(Contains(output, u8"<?php echo 'hello';?>"));
    }
}

TEST_CASE("xml.write: declaration (with/without)")
{
    XmlDocument doc;
    doc.AppendChild(DefaultAllocator().New<XmlDeclaration>(
        StringView(u8"1.0"), StringView(u8"utf-8"), StringView(u8"")));
    doc.AppendChild(doc.CreateElement(u8"root"));
    String output;
    doc.WriteTo(output);
    CHECK(
        StringView(output).StartsWith(StringView(u8"<?xml version=\"1.0\" encoding=\"utf-8\"?>")));

    XmlDocument doc2;
    doc2.AppendChild(DefaultAllocator().New<XmlDeclaration>(
        StringView(u8"1.0"), StringView(u8"utf-8"), StringView(u8"")));
    doc2.AppendChild(doc2.CreateElement(u8"root"));
    String output2;
    doc2.WriteTo(output2, NoDecl());
    CHECK_FALSE(Contains(output2, u8"<?xml"));
}

TEST_CASE("xml.write: round-trip")
{
    const char8_t* xml = u8R"xml(<?xml version="1.0" encoding="utf-8"?>
<catalog>
    <book id="1">
        <title>Test</title>
    </book>
</catalog>)xml";

    XmlDocument doc;
    REQUIRE(doc.Parse(xml) == XmlResult::Ok);
    String output;
    doc.WriteTo(output);

    XmlDocument doc2;
    REQUIRE(doc2.Parse(output) == XmlResult::Ok);
    CHECK(doc2.RootElement()->TagName() == StringView(u8"catalog"));
    XmlElement* book = doc2.RootElement()->GetFirstChildElement(u8"book");
    REQUIRE(book != nullptr);
    CHECK(book->GetAttribute(u8"id") == StringView(u8"1"));
    XmlElement* title = book->GetFirstChildElement(u8"title");
    REQUIRE(title != nullptr);
    String titleText;
    title->GetTextContent(titleText);
    CHECK(titleText == StringView(u8"Test"));
}

TEST_CASE("xml.write: ToXml(element)")
{
    XmlElement elem(u8"test");
    elem.SetAttribute(u8"id", u8"1");
    elem.SetTextContent(u8"content");
    String output;
    ToXml(elem, output, true);
    CHECK(output == StringView(u8"<test id=\"1\">content</test>"));
}

TEST_CASE("xml.write: EscapeText / EscapeAttributeValue")
{
    String output;
    XmlWriter::EscapeText(u8"a < b & c > d", output);
    CHECK(output == StringView(u8"a &lt; b &amp; c &gt; d"));

    String out2;
    XmlWriter::EscapeAttributeValue(u8"a\"b'c\r\n\t", out2);
    CHECK(Contains(out2, u8"&quot;"));
    CHECK(Contains(out2, u8"&apos;"));
    CHECK(Contains(out2, u8"&#xD;"));
    CHECK(Contains(out2, u8"&#xA;"));
    CHECK(Contains(out2, u8"&#x9;"));
}

TEST_CASE("xml.write: custom indent string")
{
    XmlDocument doc;
    XmlElement* root = doc.CreateElement(u8"root");
    root->AppendChild(doc.CreateElement(u8"child"));
    doc.AppendChild(root);
    XmlWriteSettings s = NoDecl();
    s.IndentString = u8"    ";
    String output;
    doc.WriteTo(output, s);
    CHECK(Contains(output, u8"    <child/>"));
}

TEST_CASE("xml.write: mixed content")
{
    XmlDocument doc;
    XmlElement* root = doc.CreateElement(u8"p");
    root->AppendChild(doc.CreateTextNode(u8"Hello "));
    XmlElement* bold = doc.CreateElement(u8"b");
    bold->AppendChild(doc.CreateTextNode(u8"World"));
    root->AppendChild(bold);
    root->AppendChild(doc.CreateTextNode(u8"!"));
    doc.AppendChild(root);
    XmlWriteSettings s = NoDecl();
    s.CompactMode = true;
    String output;
    doc.WriteTo(output, s);
    CHECK(output == StringView(u8"<p>Hello <b>World</b>!</p>"));
}
