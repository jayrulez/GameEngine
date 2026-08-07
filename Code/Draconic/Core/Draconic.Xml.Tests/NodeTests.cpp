// Ported from Sedulous.Xml.Tests/NodeTests.bf
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.xml;
using namespace draconic::foundation;
using namespace draconic::xml;

namespace
{
    XmlElement* NewElem(const char8_t* n)
    {
        return DefaultAllocator().New<XmlElement>(StringView(n));
    }
}

TEST_CASE("xml.node: element creation")
{
    XmlElement elem(u8"test");
    CHECK(elem.TagName() == StringView(u8"test"));
    CHECK(elem.LocalName() == StringView(u8"test"));
    CHECK(elem.Prefix().IsEmpty());
    CHECK(elem.NodeType() == XmlNodeType::Element);
}

TEST_CASE("xml.node: element creation with namespace")
{
    XmlElement elem(u8"ns", u8"test", u8"http://example.com");
    CHECK(elem.TagName() == StringView(u8"ns:test"));
    CHECK(elem.LocalName() == StringView(u8"test"));
    CHECK(elem.Prefix() == StringView(u8"ns"));
    CHECK(elem.NamespaceUri() == StringView(u8"http://example.com"));
}

TEST_CASE("xml.node: attribute manipulation")
{
    XmlElement elem(u8"test");
    elem.SetAttribute(u8"id", u8"123");
    elem.SetAttribute(u8"name", u8"value");
    CHECK(elem.AttributeCount() == 2u);
    CHECK(elem.HasAttribute(u8"id"));
    CHECK(elem.HasAttribute(u8"name"));
    CHECK(elem.GetAttribute(u8"id") == StringView(u8"123"));
    CHECK(elem.GetAttribute(u8"name") == StringView(u8"value"));

    elem.SetAttribute(u8"id", u8"456");
    CHECK(elem.GetAttribute(u8"id") == StringView(u8"456"));
    CHECK(elem.AttributeCount() == 2u);

    elem.RemoveAttribute(u8"id");
    CHECK_FALSE(elem.HasAttribute(u8"id"));
    CHECK(elem.AttributeCount() == 1u);
    CHECK(elem.GetAttribute(u8"nonexistent").IsEmpty());
}

TEST_CASE("xml.node: child manipulation")
{
    XmlElement parent(u8"parent");
    XmlElement* child1 = NewElem(u8"child1");
    XmlElement* child2 = NewElem(u8"child2");
    XmlElement* child3 = NewElem(u8"child3");

    parent.AppendChild(child1);
    parent.AppendChild(child2);
    CHECK(parent.ChildCount() == 2u);
    CHECK(parent.FirstChild() == child1);
    CHECK(parent.LastChild() == child2);
    CHECK(child1->NextSibling() == child2);
    CHECK(child2->PrevSibling() == child1);
    CHECK(child1->Parent() == &parent);

    parent.PrependChild(child3);
    CHECK(parent.FirstChild() == child3);
    CHECK(child3->NextSibling() == child1);

    XmlElement* child4 = NewElem(u8"child4");
    parent.InsertBefore(child4, child2);
    CHECK(child1->NextSibling() == child4);
    CHECK(child4->NextSibling() == child2);

    parent.RemoveChild(child4);
    DefaultAllocator().Delete(child4);
    CHECK(child1->NextSibling() == child2);
    CHECK(parent.ChildCount() == 3u);

    parent.ClearChildren();
    CHECK_FALSE(parent.HasChildren());
    CHECK(parent.ChildCount() == 0u);
}

TEST_CASE("xml.node: tree navigation")
{
    XmlElement root(u8"root");
    XmlElement* child1 = NewElem(u8"child");
    XmlElement* child2 = NewElem(u8"child");
    XmlText* text = DefaultAllocator().New<XmlText>(u8"text");
    XmlElement* child3 = NewElem(u8"other");

    root.AppendChild(child1);
    root.AppendChild(text);
    root.AppendChild(child2);
    root.AppendChild(child3);

    CHECK(root.FirstChildElement() == child1);
    CHECK(root.LastChildElement() == child3);
    CHECK(child1->NextSiblingElement() == child2);
    CHECK(child2->PrevSiblingElement() == child1);
    CHECK(child2->NextSiblingElement() == child3);
    CHECK(root.GetFirstChildElement(u8"child") == child1);
    CHECK(root.GetFirstChildElement(u8"other") == child3);
    CHECK(root.GetFirstChildElement(u8"nonexistent") == nullptr);
}

TEST_CASE("xml.node: child enumeration")
{
    XmlElement parent(u8"parent");
    parent.AppendChild(NewElem(u8"child1"));
    parent.AppendChild(NewElem(u8"child2"));
    parent.AppendChild(NewElem(u8"child3"));

    int count = 0;
    for (XmlNode* child : parent.Children())
    {
        CHECK(child->NodeType() == XmlNodeType::Element);
        ++count;
    }
    CHECK(count == 3);
}

TEST_CASE("xml.node: GetChildElements")
{
    XmlElement parent(u8"parent");
    parent.AppendChild(NewElem(u8"item"));
    parent.AppendChild(DefaultAllocator().New<XmlText>(u8"text"));
    parent.AppendChild(NewElem(u8"item"));
    parent.AppendChild(NewElem(u8"other"));

    Array<XmlElement*> items;
    parent.GetChildElements(u8"item", items);
    CHECK(items.Size() == 2u);
    items.Clear();
    parent.GetChildElements(u8"", items);
    CHECK(items.Size() == 3u);
}

TEST_CASE("xml.node: GetDescendantElements")
{
    XmlElement root(u8"root");
    XmlElement* child1 = NewElem(u8"item");
    XmlElement* child2 = NewElem(u8"container");
    XmlElement* grandchild = NewElem(u8"item");
    root.AppendChild(child1);
    root.AppendChild(child2);
    child2->AppendChild(grandchild);

    Array<XmlElement*> items;
    root.GetDescendantElements(u8"item", items);
    CHECK(items.Size() == 2u);
    CHECK(items[0] == child1);
    CHECK(items[1] == grandchild);
}

TEST_CASE("xml.node: text content")
{
    XmlElement elem(u8"test");
    elem.SetTextContent(u8"Hello World");
    CHECK(elem.HasChildren());
    CHECK(elem.ChildCount() == 1u);
    String text;
    elem.GetTextContent(text);
    CHECK(text == StringView(u8"Hello World"));

    elem.SetTextContent(u8"New Text");
    text.Clear();
    elem.GetTextContent(text);
    CHECK(text == StringView(u8"New Text"));
    CHECK(elem.ChildCount() == 1u);
}

TEST_CASE("xml.node: text node")
{
    XmlText text(u8"Hello");
    CHECK(text.Text() == StringView(u8"Hello"));
    CHECK_FALSE(text.IsWhitespace());
    text.AppendText(u8" World");
    CHECK(text.Text() == StringView(u8"Hello World"));
    XmlText wsText(u8"   \t\n");
    CHECK(wsText.IsWhitespace());
}

TEST_CASE("xml.node: CData node")
{
    XmlCData cdata(u8"<special> & content");
    CHECK(cdata.Data() == StringView(u8"<special> & content"));
    CHECK(cdata.NodeType() == XmlNodeType::CData);
    String xml;
    cdata.GetOuterXml(xml);
    CHECK(xml == StringView(u8"<![CDATA[<special> & content]]>"));
}

TEST_CASE("xml.node: comment node")
{
    XmlComment comment(u8"This is a comment");
    CHECK(comment.Text() == StringView(u8"This is a comment"));
    CHECK(comment.NodeType() == XmlNodeType::Comment);
    String xml;
    comment.GetOuterXml(xml);
    CHECK(xml == StringView(u8"<!--This is a comment-->"));
    String inner;
    comment.GetInnerText(inner);
    CHECK(inner.IsEmpty());
}

TEST_CASE("xml.node: declaration node")
{
    XmlDeclaration decl(u8"1.0", u8"utf-8", u8"yes");
    CHECK(decl.Version() == StringView(u8"1.0"));
    CHECK(decl.Encoding() == StringView(u8"utf-8"));
    CHECK(decl.Standalone() == StringView(u8"yes"));
    CHECK(decl.NodeType() == XmlNodeType::Declaration);
    String xml;
    decl.GetOuterXml(xml);
    CHECK(xml == StringView(u8"<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>"));
}

TEST_CASE("xml.node: processing instruction node")
{
    XmlProcessingInstruction pi(u8"target", u8"data content");
    CHECK(pi.Target() == StringView(u8"target"));
    CHECK(pi.Data() == StringView(u8"data content"));
    CHECK(pi.NodeType() == XmlNodeType::ProcessingInstruction);
    String xml;
    pi.GetOuterXml(xml);
    CHECK(xml == StringView(u8"<?target data content?>"));
}

TEST_CASE("xml.node: attribute node")
{
    XmlAttribute attr(u8"name", u8"value");
    CHECK(attr.Name() == StringView(u8"name"));
    CHECK(attr.Value() == StringView(u8"value"));
    CHECK(attr.LocalName() == StringView(u8"name"));
    CHECK(attr.Prefix().IsEmpty());
    CHECK_FALSE(attr.IsNamespaceDeclaration());

    XmlAttribute nsAttr(u8"xmlns:ns", u8"http://example.com");
    nsAttr.SetName(u8"xmlns:ns");
    nsAttr.SetValue(u8"http://example.com");
    CHECK(nsAttr.IsNamespaceDeclaration());
    CHECK(nsAttr.DeclaredPrefix() == StringView(u8"ns"));
}

TEST_CASE("xml.node: owner document")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root><child/></root>") == XmlResult::Ok);
    CHECK(doc.RootElement()->OwnerDocument() == &doc);
    CHECK(doc.RootElement()->FirstChild()->OwnerDocument() == &doc);
}

TEST_CASE("xml.node: remove from parent")
{
    XmlElement parent(u8"parent");
    XmlElement* child = NewElem(u8"child");
    parent.AppendChild(child);
    CHECK(child->Parent() == &parent);
    child->RemoveFromParent();
    CHECK(child->Parent() == nullptr);
    CHECK_FALSE(parent.HasChildren());
    DefaultAllocator().Delete(child);
}

TEST_CASE("xml.node: insert after")
{
    XmlElement parent(u8"parent");
    XmlElement* child1 = NewElem(u8"child1");
    XmlElement* child2 = NewElem(u8"child2");
    XmlElement* child3 = NewElem(u8"child3");
    parent.AppendChild(child1);
    parent.AppendChild(child3);
    parent.InsertAfter(child2, child1);
    CHECK(child1->NextSibling() == child2);
    CHECK(child2->NextSibling() == child3);
    CHECK(child2->PrevSibling() == child1);
}

TEST_CASE("xml.node: namespace declaration")
{
    XmlElement elem(u8"root");
    elem.SetAttribute(u8"xmlns", u8"http://default.example.com");
    elem.SetAttribute(u8"xmlns:ns", u8"http://ns.example.com");
    CHECK(elem.ResolveNamespacePrefix(u8"") == StringView(u8"http://default.example.com"));
    CHECK(elem.ResolveNamespacePrefix(u8"ns") == StringView(u8"http://ns.example.com"));
}
