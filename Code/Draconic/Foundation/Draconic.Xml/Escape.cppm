// Draconic::Xml - :escape partition
//
// XML text/attribute escaping. Shared by node GetOuterXml and the XmlWriter
// (which re-exposes these as static methods) - kept here so neither has to
// depend on the other. Ported from Sedulous.Xml/XmlWriter.bf escaping.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.xml:escape;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::xml
{
    // Escapes element text content: & < >.
    inline void EscapeText(StringView text, String& output)
    {
        for (usize i = 0; i < text.Size(); ++i)
        {
            const utf8char c = text[i];
            switch (c)
            {
            case u8'&':
                output.Append(StringView(u8"&amp;"));
                break;
            case u8'<':
                output.Append(StringView(u8"&lt;"));
                break;
            case u8'>':
                output.Append(StringView(u8"&gt;"));
                break;
            default:
                output.PushBack(c);
                break;
            }
        }
    }

    // Escapes an attribute value: & < > " ' and CR/LF/Tab as char refs.
    inline void EscapeAttributeValue(StringView value, String& output)
    {
        for (usize i = 0; i < value.Size(); ++i)
        {
            const utf8char c = value[i];
            switch (c)
            {
            case u8'&':
                output.Append(StringView(u8"&amp;"));
                break;
            case u8'<':
                output.Append(StringView(u8"&lt;"));
                break;
            case u8'>':
                output.Append(StringView(u8"&gt;"));
                break;
            case u8'"':
                output.Append(StringView(u8"&quot;"));
                break;
            case u8'\'':
                output.Append(StringView(u8"&apos;"));
                break;
            case u8'\r':
                output.Append(StringView(u8"&#xD;"));
                break;
            case u8'\n':
                output.Append(StringView(u8"&#xA;"));
                break;
            case u8'\t':
                output.Append(StringView(u8"&#x9;"));
                break;
            default:
                output.PushBack(c);
                break;
            }
        }
    }
}
