// Draconic::Xml - :document partition
//
// XmlDocument: the DOM root, recursive-descent parser, node factory, and query
// methods. Also supplies the out-of-line definitions of XmlNode::OwnerDocument
// and XmlWriter::WriteDocument (both need XmlDocument complete). Ported from
// Sedulous.Xml/XmlDocument.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.xml:document;

import draconic.foundation;
import :result;
import :lexer;
import :ns;
import :nodes;
import :writer;

using namespace draconic::foundation;

export namespace draconic::xml
{
    struct XmlParseSettings
    {
        bool PreserveWhitespace = false;
        bool IgnoreComments = false;
        bool IgnoreProcessingInstructions = false;
        bool ValidateNamespaces = true;

        [[nodiscard]] static XmlParseSettings Default() { return XmlParseSettings{}; }
    };

    class XmlDocument final : public XmlNode
    {
    public:
        XmlDocument() : XmlNode(XmlNodeType::Document) {}

        // Tracks declaration + root element as children are appended.
        void AppendChild(XmlNode* child) override
        {
            XmlNode::AppendChild(child);
            if (child->NodeType() == XmlNodeType::Declaration)
            {
                m_declaration = static_cast<XmlDeclaration*>(child);
            }
            else if (child->NodeType() == XmlNodeType::Element && m_rootElement == nullptr)
            {
                m_rootElement = static_cast<XmlElement*>(child);
            }
        }

        [[nodiscard]] XmlDeclaration* Declaration() const { return m_declaration; }
        [[nodiscard]] XmlElement* RootElement() const { return m_rootElement; }
        [[nodiscard]] i32 ErrorLine() const { return m_errorLine; }
        [[nodiscard]] i32 ErrorColumn() const { return m_errorColumn; }

        // --- parsing ---
        [[nodiscard]] XmlResult Parse(StringView text)
        {
            return Parse(text, XmlParseSettings::Default());
        }

        [[nodiscard]] XmlResult Parse(StringView text, XmlParseSettings settings)
        {
            ClearChildren();
            m_declaration = nullptr;
            m_rootElement = nullptr;
            m_errorLine = 1;
            m_errorColumn = 1;
            m_parseSettings = settings;

            StringView remaining = text;
            return ParseDocument(remaining);
        }

        // --- writing ---
        void WriteTo(String& output) const { WriteTo(output, XmlWriteSettings::Default()); }
        void WriteTo(String& output, XmlWriteSettings settings) const
        {
            XmlWriter writer(output, settings);
            writer.WriteDocument(*this);
        }

        // --- factory ---
        [[nodiscard]] XmlElement* CreateElement(StringView name)
        {
            return DefaultAllocator().New<XmlElement>(name);
        }
        [[nodiscard]] XmlElement* CreateElement(StringView prefix, StringView localName,
                                                StringView namespaceUri)
        {
            return DefaultAllocator().New<XmlElement>(prefix, localName, namespaceUri);
        }
        [[nodiscard]] XmlAttribute* CreateAttribute(StringView name)
        {
            return DefaultAllocator().New<XmlAttribute>(name, StringView(u8""));
        }
        [[nodiscard]] XmlAttribute* CreateAttribute(StringView prefix, StringView localName,
                                                    StringView namespaceUri)
        {
            return DefaultAllocator().New<XmlAttribute>(prefix, localName, namespaceUri,
                                                        StringView(u8""));
        }
        [[nodiscard]] XmlText* CreateTextNode(StringView text)
        {
            return DefaultAllocator().New<XmlText>(text);
        }
        [[nodiscard]] XmlCData* CreateCDataSection(StringView data)
        {
            return DefaultAllocator().New<XmlCData>(data);
        }
        [[nodiscard]] XmlComment* CreateComment(StringView text)
        {
            return DefaultAllocator().New<XmlComment>(text);
        }
        [[nodiscard]] XmlProcessingInstruction* CreateProcessingInstruction(StringView target,
                                                                            StringView data)
        {
            return DefaultAllocator().New<XmlProcessingInstruction>(target, data);
        }

        // --- queries ---
        void GetElementsByTagName(StringView name, Array<XmlElement*>& results) const
        {
            if (m_rootElement != nullptr)
            {
                if (name.IsEmpty() || m_rootElement->TagName() == name)
                {
                    results.PushBack(m_rootElement);
                }
                m_rootElement->GetDescendantElements(name, results);
            }
        }
        void GetElementsByTagNameNS(StringView namespaceUri, StringView localName,
                                    Array<XmlElement*>& results) const
        {
            if (m_rootElement != nullptr)
            {
                ByTagNameNS(m_rootElement, namespaceUri, localName, results);
            }
        }
        [[nodiscard]] XmlElement* GetElementById(StringView id) const
        {
            return m_rootElement != nullptr ? ById(m_rootElement, id) : nullptr;
        }

        void GetInnerText(String& output) const override
        {
            if (m_rootElement != nullptr)
            {
                m_rootElement->GetInnerText(output);
            }
        }
        void GetOuterXml(String& output) const override
        {
            XmlWriter writer(output);
            writer.WriteDocument(*this);
        }

    private:
        static void Advance(StringView& text, usize n) { text = text.SubStr(n, text.Size() - n); }
        static void SkipWhitespace(StringView& text)
        {
            Advance(text, XmlLexer::GetWhitespaceLength(text));
        }

        static bool EqualsIgnoreCaseAscii(StringView a, StringView b)
        {
            if (a.Size() != b.Size())
            {
                return false;
            }
            for (usize i = 0; i < a.Size(); ++i)
            {
                utf8char ca = a[i], cb = b[i];
                if (ca >= u8'A' && ca <= u8'Z')
                {
                    ca = static_cast<utf8char>(ca - u8'A' + u8'a');
                }
                if (cb >= u8'A' && cb <= u8'Z')
                {
                    cb = static_cast<utf8char>(cb - u8'A' + u8'a');
                }
                if (ca != cb)
                {
                    return false;
                }
            }
            return true;
        }

        static bool HasNonWhitespace(StringView text)
        {
            for (usize i = 0; i < text.Size(); ++i)
            {
                if (!XmlLexer::IsWhitespace(text[i]))
                {
                    return true;
                }
            }
            return false;
        }

        XmlResult ParseDocument(StringView& text)
        {
            SkipWhitespace(text);
            if (text.IsEmpty())
            {
                return XmlResult::NoRootElement;
            }

            if (text.StartsWith(StringView(u8"<?xml")))
            {
                const XmlResult r = ParseDeclaration(text);
                if (r != XmlResult::Ok)
                {
                    return r;
                }
                SkipWhitespace(text);
            }

            // Misc (comments/PIs) before the root element.
            const XmlResult before = ParseMisc(text);
            if (before != XmlResult::Ok)
            {
                return before;
            }

            if (text.IsEmpty() || !text.StartsWith(StringView(u8"<")))
            {
                return XmlResult::NoRootElement;
            }
            if (text.StartsWith(StringView(u8"</")))
            {
                return XmlResult::TagUnexpectedClose;
            }

            const XmlResult rootResult = ParseElement(text, this);
            if (rootResult != XmlResult::Ok)
            {
                return rootResult;
            }

            // Misc after the root element.
            SkipWhitespace(text);
            while (!text.IsEmpty())
            {
                if (text.StartsWith(StringView(u8"<!--")) || text.StartsWith(StringView(u8"<?")))
                {
                    const XmlResult r = ParseMiscItem(text);
                    if (r != XmlResult::Ok)
                    {
                        return r;
                    }
                    SkipWhitespace(text);
                }
                else if (text.StartsWith(StringView(u8"<")))
                {
                    return XmlResult::MultipleRoots;
                }
                else
                {
                    return HasNonWhitespace(text) ? XmlResult::ContentAfterRoot : XmlResult::Ok;
                }
            }
            return XmlResult::Ok;
        }

        // Parses leading comments/PIs until a non-misc token; returns Ok at that point.
        XmlResult ParseMisc(StringView& text)
        {
            while (!text.IsEmpty())
            {
                if (text.StartsWith(StringView(u8"<!--")) || text.StartsWith(StringView(u8"<?")))
                {
                    const XmlResult r = ParseMiscItem(text);
                    if (r != XmlResult::Ok)
                    {
                        return r;
                    }
                    SkipWhitespace(text);
                }
                else
                {
                    break;
                }
            }
            return XmlResult::Ok;
        }

        XmlResult ParseMiscItem(StringView& text)
        {
            if (text.StartsWith(StringView(u8"<!--")))
            {
                return m_parseSettings.IgnoreComments ? SkipComment(text)
                                                      : ParseComment(text, this);
            }
            // "<?"
            return m_parseSettings.IgnoreProcessingInstructions
                       ? SkipProcessingInstruction(text)
                       : ParseProcessingInstruction(text, this);
        }

        XmlResult ParseDeclaration(StringView& text)
        {
            if (!text.StartsWith(StringView(u8"<?xml")))
            {
                return XmlResult::DeclarationInvalid;
            }
            Advance(text, 5);
            SkipWhitespace(text);

            XmlDeclaration* declaration = DefaultAllocator().New<XmlDeclaration>();

            // version (required)
            if (!text.StartsWith(StringView(u8"version")))
            {
                DefaultAllocator().Delete(declaration);
                return XmlResult::DeclarationVersion;
            }
            Advance(text, 7);
            SkipWhitespace(text);
            if (text.IsEmpty() || text[0] != u8'=')
            {
                DefaultAllocator().Delete(declaration);
                return XmlResult::DeclarationVersion;
            }
            Advance(text, 1);
            SkipWhitespace(text);
            String versionStr;
            usize versionLen = 0;
            if (XmlLexer::ReadAttributeValue(text, versionLen, versionStr) != XmlResult::Ok)
            {
                DefaultAllocator().Delete(declaration);
                return XmlResult::DeclarationVersion;
            }
            Advance(text, versionLen);
            declaration->SetVersion(versionStr);
            SkipWhitespace(text);

            // optional encoding
            if (text.StartsWith(StringView(u8"encoding")))
            {
                Advance(text, 8);
                SkipWhitespace(text);
                if (text.IsEmpty() || text[0] != u8'=')
                {
                    DefaultAllocator().Delete(declaration);
                    return XmlResult::DeclarationInvalid;
                }
                Advance(text, 1);
                SkipWhitespace(text);
                String encodingStr;
                usize encodingLen = 0;
                if (XmlLexer::ReadAttributeValue(text, encodingLen, encodingStr) != XmlResult::Ok)
                {
                    DefaultAllocator().Delete(declaration);
                    return XmlResult::DeclarationInvalid;
                }
                Advance(text, encodingLen);
                declaration->SetEncoding(encodingStr);
                SkipWhitespace(text);
            }

            // optional standalone
            if (text.StartsWith(StringView(u8"standalone")))
            {
                Advance(text, 10);
                SkipWhitespace(text);
                if (text.IsEmpty() || text[0] != u8'=')
                {
                    DefaultAllocator().Delete(declaration);
                    return XmlResult::DeclarationInvalid;
                }
                Advance(text, 1);
                SkipWhitespace(text);
                String standaloneStr;
                usize standaloneLen = 0;
                if (XmlLexer::ReadAttributeValue(text, standaloneLen, standaloneStr) !=
                    XmlResult::Ok)
                {
                    DefaultAllocator().Delete(declaration);
                    return XmlResult::DeclarationInvalid;
                }
                Advance(text, standaloneLen);
                declaration->SetStandalone(standaloneStr);
                SkipWhitespace(text);
            }

            if (!text.StartsWith(StringView(u8"?>")))
            {
                DefaultAllocator().Delete(declaration);
                return XmlResult::DeclarationInvalid;
            }
            Advance(text, 2);
            AppendChild(declaration); // success: attach (sets m_declaration)
            return XmlResult::Ok;
        }

        XmlResult ParseElement(StringView& text, XmlNode* parent)
        {
            if (!text.StartsWith(StringView(u8"<")))
            {
                return XmlResult::SyntaxError;
            }
            Advance(text, 1);

            String tagName;
            usize nameLen = 0;
            if (XmlLexer::ReadName(text, nameLen, tagName) != XmlResult::Ok)
            {
                return XmlResult::TagInvalid;
            }
            Advance(text, nameLen);

            XmlElement* element = DefaultAllocator().New<XmlElement>(StringView(tagName));

            const XmlResult attrResult = ParseAttributes(text, element);
            if (attrResult != XmlResult::Ok)
            {
                DefaultAllocator().Delete(element);
                return attrResult;
            }

            SkipWhitespace(text);

            if (text.StartsWith(StringView(u8"/>")))
            {
                Advance(text, 2);
                parent->AppendChild(element);
                return XmlResult::Ok;
            }

            if (text.IsEmpty() || text[0] != u8'>')
            {
                DefaultAllocator().Delete(element);
                return XmlResult::TagUnclosed;
            }
            Advance(text, 1);
            parent->AppendChild(element);

            const XmlResult contentResult = ParseContent(text, element);
            if (contentResult != XmlResult::Ok)
            {
                return contentResult;
            }

            if (!text.StartsWith(StringView(u8"</")))
            {
                return XmlResult::TagUnclosed;
            }
            Advance(text, 2);

            String closingName;
            usize closingLen = 0;
            if (XmlLexer::ReadName(text, closingLen, closingName) != XmlResult::Ok)
            {
                return XmlResult::TagInvalid;
            }
            Advance(text, closingLen);

            if (StringView(closingName) != StringView(tagName))
            {
                return XmlResult::TagMismatch;
            }

            SkipWhitespace(text);
            if (text.IsEmpty() || text[0] != u8'>')
            {
                return XmlResult::TagUnclosed;
            }
            Advance(text, 1);
            return XmlResult::Ok;
        }

        XmlResult ParseAttributes(StringView& text, XmlElement* element)
        {
            while (true)
            {
                SkipWhitespace(text);
                if (text.IsEmpty())
                {
                    return XmlResult::UnexpectedEndOfFile;
                }
                if (text[0] == u8'>' || text.StartsWith(StringView(u8"/>")))
                {
                    return XmlResult::Ok;
                }

                String attrName;
                usize nameLen = 0;
                if (XmlLexer::ReadName(text, nameLen, attrName) != XmlResult::Ok)
                {
                    return XmlResult::AttributeInvalid;
                }
                Advance(text, nameLen);
                SkipWhitespace(text);

                if (element->HasAttribute(attrName))
                {
                    return XmlResult::AttributeDuplicate;
                }

                if (text.IsEmpty() || text[0] != u8'=')
                {
                    return XmlResult::AttributeMissingEquals;
                }
                Advance(text, 1);
                SkipWhitespace(text);

                String attrValue;
                usize valueLen = 0;
                const XmlResult valueResult =
                    XmlLexer::ReadAttributeValue(text, valueLen, attrValue);
                if (valueResult != XmlResult::Ok)
                {
                    return valueResult;
                }
                Advance(text, valueLen);

                element->SetAttribute(attrName, attrValue);
            }
        }

        XmlResult ParseContent(StringView& text, XmlElement* parent)
        {
            while (!text.IsEmpty())
            {
                if (text.StartsWith(StringView(u8"</")))
                {
                    return XmlResult::Ok;
                }
                if (text.StartsWith(StringView(u8"<![CDATA[")))
                {
                    const XmlResult r = ParseCData(text, parent);
                    if (r != XmlResult::Ok)
                    {
                        return r;
                    }
                }
                else if (text.StartsWith(StringView(u8"<!--")) ||
                         text.StartsWith(StringView(u8"<?")))
                {
                    const XmlResult r = ParseMiscItem(text, parent);
                    if (r != XmlResult::Ok)
                    {
                        return r;
                    }
                }
                else if (text.StartsWith(StringView(u8"<")))
                {
                    const XmlResult r = ParseElement(text, parent);
                    if (r != XmlResult::Ok)
                    {
                        return r;
                    }
                }
                else
                {
                    const XmlResult r = ParseTextContent(text, parent);
                    if (r != XmlResult::Ok)
                    {
                        return r;
                    }
                }
            }
            return XmlResult::UnexpectedEndOfFile;
        }

        // Misc item attached to an arbitrary parent (content position).
        XmlResult ParseMiscItem(StringView& text, XmlNode* parent)
        {
            if (text.StartsWith(StringView(u8"<!--")))
            {
                return m_parseSettings.IgnoreComments ? SkipComment(text)
                                                      : ParseComment(text, parent);
            }
            return m_parseSettings.IgnoreProcessingInstructions
                       ? SkipProcessingInstruction(text)
                       : ParseProcessingInstruction(text, parent);
        }

        XmlResult ParseTextContent(StringView& text, XmlNode* parent)
        {
            String content;
            usize len = 0;
            const XmlResult r = XmlLexer::ReadTextContent(text, len, content);
            if (r != XmlResult::Ok)
            {
                return r;
            }
            Advance(text, len);

            bool isWhitespace = true;
            for (usize i = 0; i < content.Size(); ++i)
            {
                if (!XmlLexer::IsWhitespace(content[i]))
                {
                    isWhitespace = false;
                    break;
                }
            }
            if (isWhitespace && !m_parseSettings.PreserveWhitespace)
            {
                return XmlResult::Ok;
            }
            if (content.IsEmpty())
            {
                return XmlResult::Ok;
            }

            parent->AppendChild(DefaultAllocator().New<XmlText>(StringView(content)));
            return XmlResult::Ok;
        }

        XmlResult ParseCData(StringView& text, XmlNode* parent)
        {
            if (!text.StartsWith(StringView(u8"<![CDATA[")))
            {
                return XmlResult::CDataMalformed;
            }
            Advance(text, 9);
            String content;
            usize len = 0;
            const XmlResult r = XmlLexer::ReadCDataContent(text, len, content);
            if (r != XmlResult::Ok)
            {
                return r;
            }
            Advance(text, len);
            parent->AppendChild(DefaultAllocator().New<XmlCData>(StringView(content)));
            return XmlResult::Ok;
        }

        XmlResult ParseComment(StringView& text, XmlNode* parent)
        {
            if (!text.StartsWith(StringView(u8"<!--")))
            {
                return XmlResult::CommentMalformed;
            }
            Advance(text, 4);
            String content;
            usize len = 0;
            const XmlResult r = XmlLexer::ReadCommentContent(text, len, content);
            if (r != XmlResult::Ok)
            {
                return r;
            }
            Advance(text, len);
            parent->AppendChild(DefaultAllocator().New<XmlComment>(StringView(content)));
            return XmlResult::Ok;
        }

        XmlResult SkipComment(StringView& text)
        {
            if (!text.StartsWith(StringView(u8"<!--")))
            {
                return XmlResult::CommentMalformed;
            }
            Advance(text, 4);
            String content;
            usize len = 0;
            const XmlResult r = XmlLexer::ReadCommentContent(text, len, content);
            if (r != XmlResult::Ok)
            {
                return r;
            }
            Advance(text, len);
            return XmlResult::Ok;
        }

        XmlResult ParseProcessingInstruction(StringView& text, XmlNode* parent)
        {
            if (!text.StartsWith(StringView(u8"<?")))
            {
                return XmlResult::PIInvalid;
            }
            Advance(text, 2);
            String target, data;
            usize len = 0;
            const XmlResult r = XmlLexer::ReadProcessingInstruction(text, len, target, data);
            if (r != XmlResult::Ok)
            {
                return r;
            }
            Advance(text, len);
            if (EqualsIgnoreCaseAscii(target, StringView(u8"xml")))
            {
                return XmlResult::DeclarationPosition;
            }
            parent->AppendChild(DefaultAllocator().New<XmlProcessingInstruction>(StringView(target),
                                                                                 StringView(data)));
            return XmlResult::Ok;
        }

        XmlResult SkipProcessingInstruction(StringView& text)
        {
            if (!text.StartsWith(StringView(u8"<?")))
            {
                return XmlResult::PIInvalid;
            }
            Advance(text, 2);
            String target, data;
            usize len = 0;
            const XmlResult r = XmlLexer::ReadProcessingInstruction(text, len, target, data);
            if (r != XmlResult::Ok)
            {
                return r;
            }
            Advance(text, len);
            return XmlResult::Ok;
        }

        static void ByTagNameNS(XmlElement* element, StringView nsUri, StringView localName,
                                Array<XmlElement*>& results)
        {
            if ((nsUri.IsEmpty() || element->NamespaceUri() == nsUri) &&
                (localName.IsEmpty() || element->LocalName() == localName))
            {
                results.PushBack(element);
            }
            for (XmlNode* c = element->FirstChild(); c != nullptr; c = c->NextSibling())
            {
                if (c->NodeType() == XmlNodeType::Element)
                {
                    ByTagNameNS(static_cast<XmlElement*>(c), nsUri, localName, results);
                }
            }
        }
        static XmlElement* ById(XmlElement* element, StringView id)
        {
            if (element->GetAttribute(StringView(u8"id")) == id)
            {
                return element;
            }
            for (XmlNode* c = element->FirstChild(); c != nullptr; c = c->NextSibling())
            {
                if (c->NodeType() == XmlNodeType::Element)
                {
                    if (XmlElement* found = ById(static_cast<XmlElement*>(c), id))
                    {
                        return found;
                    }
                }
            }
            return nullptr;
        }

        XmlDeclaration* m_declaration = nullptr;
        XmlElement* m_rootElement = nullptr;
        XmlParseSettings m_parseSettings = XmlParseSettings::Default();
        i32 m_errorLine = 1;
        i32 m_errorColumn = 1;
    };

    // ToXml for a whole document.
    inline void ToXml(const XmlDocument& doc, String& output, bool compact = false)
    {
        XmlWriteSettings settings = XmlWriteSettings::Default();
        settings.CompactMode = compact;
        XmlWriter writer(output, settings);
        writer.WriteDocument(doc);
    }
}

// ---- Out-of-line definitions that need XmlDocument complete ----
namespace draconic::xml
{
    XmlDocument* XmlNode::OwnerDocument() const
    {
        const XmlNode* node = this;
        while (node != nullptr)
        {
            if (node->NodeType() == XmlNodeType::Document)
            {
                return static_cast<XmlDocument*>(const_cast<XmlNode*>(node));
            }
            node = node->Parent();
        }
        return nullptr;
    }

    void XmlWriter::WriteDocument(const XmlDocument& document)
    {
        if (!m_settings.OmitDeclaration && document.Declaration() != nullptr)
        {
            WriteDeclaration(*document.Declaration());
            WriteNewLine();
        }
        for (XmlNode* child = document.FirstChild(); child != nullptr; child = child->NextSibling())
        {
            if (child->NodeType() != XmlNodeType::Declaration)
            {
                WriteNode(*child);
            }
        }
    }
}
