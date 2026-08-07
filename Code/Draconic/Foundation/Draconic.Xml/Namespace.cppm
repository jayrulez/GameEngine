// Draconic::Xml - :namespace partition
//
// Well-known XML namespace constants + namespace-declaration helpers. Ported
// from Sedulous.Xml/XmlNamespace.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.xml:ns;

import draconic.foundation;
import :result;
import :lexer;

using namespace draconic::foundation;

export namespace draconic::xml
{
    namespace XmlNamespaces
    {
        inline constexpr StringView Xml = StringView(u8"http://www.w3.org/XML/1998/namespace");
        inline constexpr StringView Xmlns = StringView(u8"http://www.w3.org/2000/xmlns/");
        inline constexpr StringView XmlPrefix = StringView(u8"xml");
        inline constexpr StringView XmlnsPrefix = StringView(u8"xmlns");
    }

    class XmlNamespaceHelper
    {
    public:
        static void SplitQualifiedName(StringView qualifiedName, String& prefix, String& localName)
        {
            XmlLexer::SplitQualifiedName(qualifiedName, prefix, localName);
        }

        [[nodiscard]] static bool IsReservedPrefix(StringView prefix)
        {
            return prefix == XmlNamespaces::XmlPrefix || prefix == XmlNamespaces::XmlnsPrefix;
        }

        [[nodiscard]] static XmlResult ValidateNamespaceDeclaration(StringView prefix,
                                                                    StringView uri)
        {
            // "xml" may only be bound to the XML namespace.
            if (prefix == StringView(u8"xml") && uri != XmlNamespaces::Xml)
            {
                return XmlResult::PrefixReserved;
            }
            // "xmlns" cannot be declared.
            if (prefix == StringView(u8"xmlns"))
            {
                return XmlResult::PrefixReserved;
            }
            // The XML namespace can only bind to "xml".
            if (uri == XmlNamespaces::Xml && prefix != StringView(u8"xml"))
            {
                return XmlResult::NamespaceInvalid;
            }
            // The XMLNS namespace cannot bind to any prefix.
            if (uri == XmlNamespaces::Xmlns)
            {
                return XmlResult::NamespaceInvalid;
            }
            return XmlResult::Ok;
        }

        // Names starting with "xml" (any case) are reserved.
        [[nodiscard]] static bool StartsWithXml(StringView name)
        {
            if (name.Size() < 3)
            {
                return false;
            }
            const utf8char c0 = name[0], c1 = name[1], c2 = name[2];
            return (c0 == u8'x' || c0 == u8'X') && (c1 == u8'm' || c1 == u8'M') &&
                   (c2 == u8'l' || c2 == u8'L');
        }
    };
}
