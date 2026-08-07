// Ported from Sedulous.Xml.Tests/NamespaceTests.bf
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.xml;
using namespace draconic::foundation;
using namespace draconic::xml;

TEST_CASE("xml.ns: default namespace")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root xmlns=\"http://example.com\"/>") == XmlResult::Ok);
    CHECK(doc.RootElement()->HasAttribute(u8"xmlns"));
    CHECK(doc.RootElement()->GetAttribute(u8"xmlns") == StringView(u8"http://example.com"));
    CHECK(doc.RootElement()->ResolveNamespacePrefix(u8"") == StringView(u8"http://example.com"));
}

TEST_CASE("xml.ns: prefixed namespace")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<ns:root xmlns:ns=\"http://example.com\"/>") == XmlResult::Ok);
    CHECK(doc.RootElement()->TagName() == StringView(u8"ns:root"));
    CHECK(doc.RootElement()->Prefix() == StringView(u8"ns"));
    CHECK(doc.RootElement()->LocalName() == StringView(u8"root"));
    CHECK(doc.RootElement()->ResolveNamespacePrefix(u8"ns") == StringView(u8"http://example.com"));
}

TEST_CASE("xml.ns: inheritance")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8R"(<root xmlns:ns="http://example.com"><ns:child/></root>)") ==
            XmlResult::Ok);
    XmlElement* child = doc.RootElement()->FirstChildElement();
    REQUIRE(child != nullptr);
    CHECK(child->TagName() == StringView(u8"ns:child"));
    CHECK(child->ResolveNamespacePrefix(u8"ns") == StringView(u8"http://example.com"));
}

TEST_CASE("xml.ns: override")
{
    XmlDocument doc;
    REQUIRE(
        doc.Parse(
            u8R"(<root xmlns:ns="http://outer.com"><child xmlns:ns="http://inner.com"><ns:leaf/></child></root>)") ==
        XmlResult::Ok);
    XmlElement* child = doc.RootElement()->FirstChildElement();
    REQUIRE(child != nullptr);
    CHECK(child->ResolveNamespacePrefix(u8"ns") == StringView(u8"http://inner.com"));
    CHECK(doc.RootElement()->ResolveNamespacePrefix(u8"ns") == StringView(u8"http://outer.com"));
}

TEST_CASE("xml.ns: reserved prefixes")
{
    XmlElement elem(u8"test");
    CHECK(elem.ResolveNamespacePrefix(u8"xml") == XmlNamespaces::Xml);
    CHECK(elem.ResolveNamespacePrefix(u8"xmlns") == XmlNamespaces::Xmlns);
}

TEST_CASE("xml.ns: attribute namespaces")
{
    XmlDocument doc;
    REQUIRE(doc.Parse(u8"<root xmlns:ns=\"http://example.com\" ns:attr=\"value\"/>") ==
            XmlResult::Ok);
    XmlAttribute* attr = doc.RootElement()->GetAttributeNode(u8"ns:attr");
    REQUIRE(attr != nullptr);
    CHECK(attr->Prefix() == StringView(u8"ns"));
    CHECK(attr->LocalName() == StringView(u8"attr"));
}

TEST_CASE("xml.ns: IsNamespaceDeclaration")
{
    XmlAttribute xmlnsAttr(u8"xmlns", u8"http://default.com");
    CHECK(xmlnsAttr.IsNamespaceDeclaration());
    CHECK(xmlnsAttr.DeclaredPrefix().IsEmpty());
    CHECK(xmlnsAttr.DeclaredNamespaceUri() == StringView(u8"http://default.com"));

    XmlAttribute prefixedAttr(u8"xmlns:ns", u8"http://ns.com");
    CHECK(prefixedAttr.IsNamespaceDeclaration());
    CHECK(prefixedAttr.DeclaredPrefix() == StringView(u8"ns"));
    CHECK(prefixedAttr.DeclaredNamespaceUri() == StringView(u8"http://ns.com"));

    XmlAttribute normalAttr(u8"id", u8"123");
    CHECK_FALSE(normalAttr.IsNamespaceDeclaration());
}

TEST_CASE("xml.ns: helper IsReservedPrefix / Validate / Split / StartsWithXml")
{
    CHECK(XmlNamespaceHelper::IsReservedPrefix(u8"xml"));
    CHECK(XmlNamespaceHelper::IsReservedPrefix(u8"xmlns"));
    CHECK_FALSE(XmlNamespaceHelper::IsReservedPrefix(u8"myprefix"));
    CHECK_FALSE(XmlNamespaceHelper::IsReservedPrefix(u8""));

    CHECK(XmlNamespaceHelper::ValidateNamespaceDeclaration(u8"ns", u8"http://example.com") ==
          XmlResult::Ok);
    CHECK(XmlNamespaceHelper::ValidateNamespaceDeclaration(u8"", u8"http://default.com") ==
          XmlResult::Ok);
    CHECK(XmlNamespaceHelper::ValidateNamespaceDeclaration(u8"xmlns", u8"http://example.com") ==
          XmlResult::PrefixReserved);
    CHECK(XmlNamespaceHelper::ValidateNamespaceDeclaration(u8"xml", u8"http://other.com") ==
          XmlResult::PrefixReserved);
    CHECK(XmlNamespaceHelper::ValidateNamespaceDeclaration(u8"xml", XmlNamespaces::Xml) ==
          XmlResult::Ok);
    CHECK(XmlNamespaceHelper::ValidateNamespaceDeclaration(u8"other", XmlNamespaces::Xml) ==
          XmlResult::NamespaceInvalid);
    CHECK(XmlNamespaceHelper::ValidateNamespaceDeclaration(u8"ns", XmlNamespaces::Xmlns) ==
          XmlResult::NamespaceInvalid);

    String prefix, localName;
    XmlNamespaceHelper::SplitQualifiedName(u8"element", prefix, localName);
    CHECK(prefix.IsEmpty());
    CHECK(localName == StringView(u8"element"));
    prefix.Clear();
    localName.Clear();
    XmlNamespaceHelper::SplitQualifiedName(u8"ns:element", prefix, localName);
    CHECK(prefix == StringView(u8"ns"));
    CHECK(localName == StringView(u8"element"));

    CHECK(XmlNamespaceHelper::StartsWithXml(u8"xml"));
    CHECK(XmlNamespaceHelper::StartsWithXml(u8"XML"));
    CHECK(XmlNamespaceHelper::StartsWithXml(u8"Xml"));
    CHECK(XmlNamespaceHelper::StartsWithXml(u8"xmlfoo"));
    CHECK_FALSE(XmlNamespaceHelper::StartsWithXml(u8"xm"));
    CHECK_FALSE(XmlNamespaceHelper::StartsWithXml(u8"foo"));
    CHECK_FALSE(XmlNamespaceHelper::StartsWithXml(u8""));
}

TEST_CASE("xml.ns: constants")
{
    CHECK(XmlNamespaces::Xml == StringView(u8"http://www.w3.org/XML/1998/namespace"));
    CHECK(XmlNamespaces::Xmlns == StringView(u8"http://www.w3.org/2000/xmlns/"));
    CHECK(XmlNamespaces::XmlPrefix == StringView(u8"xml"));
    CHECK(XmlNamespaces::XmlnsPrefix == StringView(u8"xmlns"));
}

TEST_CASE("xml.ns: ResolveNamespaceUri")
{
    XmlElement elem(u8"test");
    elem.DeclareNamespace(u8"ns", u8"http://example.com");
    CHECK(elem.ResolveNamespaceUri(u8"http://example.com") == StringView(u8"ns"));
    CHECK(elem.ResolveNamespaceUri(u8"http://other.com").IsEmpty());
    CHECK(elem.ResolveNamespaceUri(XmlNamespaces::Xml) == StringView(u8"xml"));
    CHECK(elem.ResolveNamespaceUri(XmlNamespaces::Xmlns) == StringView(u8"xmlns"));
}

TEST_CASE("xml.ns: multiple namespaces")
{
    XmlDocument doc;
    REQUIRE(
        doc.Parse(
            u8R"(<root xmlns="http://default.com" xmlns:a="http://a.com" xmlns:b="http://b.com"><a:element/><b:element/></root>)") ==
        XmlResult::Ok);
    XmlElement* root = doc.RootElement();
    CHECK(root->ResolveNamespacePrefix(u8"") == StringView(u8"http://default.com"));
    CHECK(root->ResolveNamespacePrefix(u8"a") == StringView(u8"http://a.com"));
    CHECK(root->ResolveNamespacePrefix(u8"b") == StringView(u8"http://b.com"));
}

TEST_CASE("xml.ns: programmatic namespace")
{
    XmlDocument doc;
    XmlElement* root = doc.CreateElement(u8"ns", u8"root", u8"http://example.com");
    root->SetAttribute(u8"xmlns:ns", u8"http://example.com");
    doc.AppendChild(root);
    CHECK(root->TagName() == StringView(u8"ns:root"));
    CHECK(root->Prefix() == StringView(u8"ns"));
    CHECK(root->LocalName() == StringView(u8"root"));
    CHECK(root->NamespaceUri() == StringView(u8"http://example.com"));
}
