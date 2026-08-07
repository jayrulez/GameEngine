// Ported from Sedulous.Xml.Tests/ParserTests.bf
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.xml;
using namespace draconic::foundation;
using namespace draconic::xml;

TEST_CASE("xml.parse: empty + self-closing element")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root></root>") == XmlResult::Ok);
    REQUIRE(doc.RootElement() != nullptr);
    CHECK(doc.RootElement()->TagName() == StringView(u8"root"));
    CHECK_FALSE(doc.RootElement()->HasChildren());

    XmlDocument doc2;
    REQUIRE(doc2.Parse(u8"<root/>") == XmlResult::Ok);
    CHECK(doc2.RootElement()->TagName() == StringView(u8"root"));
    CHECK_FALSE(doc2.RootElement()->HasChildren());
}

TEST_CASE("xml.parse: element with text")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root>Hello World</root>") == XmlResult::Ok);
    CHECK(doc.RootElement()->HasChildren());
    String text;
    doc.RootElement()->GetTextContent(text);
    CHECK(text == StringView(u8"Hello World"));
}

TEST_CASE("xml.parse: element with attributes")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root id=\"123\" name=\"test\"/>") == XmlResult::Ok);
    CHECK(doc.RootElement()->AttributeCount() == 2u);
    CHECK(doc.RootElement()->GetAttribute(u8"id") == StringView(u8"123"));
    CHECK(doc.RootElement()->GetAttribute(u8"name") == StringView(u8"test"));
}

TEST_CASE("xml.parse: nested elements")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root><child1/><child2><grandchild/></child2></root>") == XmlResult::Ok);
    CHECK(doc.RootElement()->ChildCount() == 2u);
    CHECK(doc.RootElement()->GetFirstChildElement(u8"child1") != nullptr);
    XmlElement* child2 = doc.RootElement()->GetFirstChildElement(u8"child2");
    REQUIRE(child2 != nullptr);
    CHECK(child2->HasChildren());
    CHECK(child2->GetFirstChildElement(u8"grandchild") != nullptr);
}

TEST_CASE("xml.parse: declaration")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<?xml version=\"1.0\"?><root/>") == XmlResult::Ok);
    REQUIRE(doc.Declaration() != nullptr);
    CHECK(doc.Declaration()->Version() == StringView(u8"1.0"));

    XmlDocument doc2;
    REQUIRE(doc2.Parse(u8"<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?><root/>") ==
            XmlResult::Ok);
    CHECK(doc2.Declaration()->Version() == StringView(u8"1.0"));
    CHECK(doc2.Declaration()->Encoding() == StringView(u8"utf-8"));
    CHECK(doc2.Declaration()->Standalone() == StringView(u8"yes"));
}

TEST_CASE("xml.parse: CDATA")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root><![CDATA[<special> & content]]></root>") == XmlResult::Ok);
    CHECK(doc.RootElement()->HasChildren());
    String text;
    doc.RootElement()->GetTextContent(text);
    CHECK(text == StringView(u8"<special> & content"));
}

TEST_CASE("xml.parse: comment")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root><!-- this is a comment --></root>") == XmlResult::Ok);
    bool found = false;
    for (XmlNode* child : doc.RootElement()->Children())
    {
        if (child->NodeType() == XmlNodeType::Comment)
        {
            CHECK(static_cast<XmlComment*>(child)->Text() == StringView(u8" this is a comment "));
            found = true;
        }
    }
    CHECK(found);

    XmlDocument doc2;
    XmlParseSettings settings = XmlParseSettings::Default();
    settings.IgnoreComments = true;
    REQUIRE(doc2.Parse(u8"<root><!-- comment --></root>", settings) == XmlResult::Ok);
    CHECK_FALSE(doc2.RootElement()->HasChildren());
}

TEST_CASE("xml.parse: processing instruction")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root><?target data?></root>") == XmlResult::Ok);
    bool found = false;
    for (XmlNode* child : doc.RootElement()->Children())
    {
        if (child->NodeType() == XmlNodeType::ProcessingInstruction)
        {
            XmlProcessingInstruction* pi = static_cast<XmlProcessingInstruction*>(child);
            CHECK(pi->Target() == StringView(u8"target"));
            CHECK(pi->Data() == StringView(u8"data"));
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("xml.parse: mixed content + preserve whitespace")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root>text1<child/>text2</root>") == XmlResult::Ok);

    XmlParseSettings settings = XmlParseSettings::Default();
    settings.PreserveWhitespace = true;
    XmlDocument doc2;
    REQUIRE(doc2.Parse(u8"<root>text1<child/>text2</root>", settings) == XmlResult::Ok);
    CHECK(doc2.RootElement()->ChildCount() == 3u);
}

TEST_CASE("xml.parse: built-in entities + char refs")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root>&amp;&lt;&gt;&apos;&quot;</root>") == XmlResult::Ok);
    String text;
    doc.RootElement()->GetTextContent(text);
    CHECK(text == StringView(u8"&<>'\""));

    XmlDocument doc2;
    REQUIRE(doc2.Parse(u8"<root>&#65;&#x42;</root>") == XmlResult::Ok);
    String text2;
    doc2.RootElement()->GetTextContent(text2);
    CHECK(text2 == StringView(u8"AB"));
}

TEST_CASE("xml.parse: namespaces")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root xmlns=\"http://example.com\"/>") == XmlResult::Ok);
    CHECK(doc.RootElement()->HasAttribute(u8"xmlns"));
    CHECK(doc.RootElement()->GetAttribute(u8"xmlns") == StringView(u8"http://example.com"));

    XmlDocument doc2;
    REQUIRE(doc2.Parse(u8"<ns:root xmlns:ns=\"http://example.com\"/>") == XmlResult::Ok);
    CHECK(doc2.RootElement()->TagName() == StringView(u8"ns:root"));
    CHECK(doc2.RootElement()->Prefix() == StringView(u8"ns"));
    CHECK(doc2.RootElement()->LocalName() == StringView(u8"root"));
}

TEST_CASE("xml.parse: error cases")
{
    {
        XmlDocument d;
        CHECK(d.Parse(u8"<root></other>") == XmlResult::TagMismatch);
    }
    {
        XmlDocument d;
        CHECK(d.Parse(u8"<root id=\"1\" id=\"2\"/>") == XmlResult::AttributeDuplicate);
    }
    {
        XmlDocument d;
        CHECK(d.Parse(u8"<root><child></root>") == XmlResult::TagMismatch);
    }
    {
        XmlDocument d;
        CHECK(d.Parse(u8"<root1/><root2/>") == XmlResult::MultipleRoots);
    }
    {
        XmlDocument d;
        CHECK(d.Parse(u8"") == XmlResult::NoRootElement);
    }
    {
        XmlDocument d;
        CHECK(d.Parse(u8"<!-- comment only -->") == XmlResult::NoRootElement);
    }
    {
        XmlDocument d;
        CHECK(d.Parse(u8"<root>&unknown;</root>") == XmlResult::EntityUnknown);
    }
    {
        XmlDocument d;
        CHECK(d.Parse(u8"<root><![CDATA[unclosed</root>") == XmlResult::CDataUnclosed);
    }
    {
        XmlDocument d;
        CHECK(d.Parse(u8"<root><!-- unclosed</root>") == XmlResult::CommentUnclosed);
    }
}

TEST_CASE("xml.parse: whitespace handling")
{
    XmlParseSettings settings = XmlParseSettings::Default();
    settings.PreserveWhitespace = false;
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root>  <child/>  </root>", settings) == XmlResult::Ok);
    CHECK(doc.RootElement()->ChildCount() == 1u);

    settings.PreserveWhitespace = true;
    XmlDocument doc2;
    REQUIRE(doc2.Parse(u8"<root>  <child/>  </root>", settings) == XmlResult::Ok);
    CHECK(doc2.RootElement()->ChildCount() == 3u);
}

TEST_CASE("xml.parse: complex document")
{
    const char8_t* xml = u8R"xml(<?xml version="1.0" encoding="utf-8"?>
<!-- Root comment -->
<catalog>
    <book id="1">
        <title>XML Guide</title>
        <author>John Doe</author>
        <price>29.99</price>
    </book>
    <book id="2">
        <title>Advanced XML</title>
        <author>Jane Smith</author>
        <price>39.99</price>
    </book>
</catalog>)xml";

    XmlDocument doc;
    REQUIRE(doc.Parse(xml) == XmlResult::Ok);
    REQUIRE(doc.Declaration() != nullptr);
    CHECK(doc.RootElement()->TagName() == StringView(u8"catalog"));

    Array<XmlElement*> books;
    doc.GetElementsByTagName(u8"book", books);
    CHECK(books.Size() == 2u);
    CHECK(books[0]->GetAttribute(u8"id") == StringView(u8"1"));
    XmlElement* title = books[0]->GetFirstChildElement(u8"title");
    REQUIRE(title != nullptr);
    String titleText;
    title->GetTextContent(titleText);
    CHECK(titleText == StringView(u8"XML Guide"));
}
