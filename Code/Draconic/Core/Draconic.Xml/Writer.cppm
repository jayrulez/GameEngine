// Draconic::Xml - :writer partition
//
// XmlWriter: serializes a node/tree to UTF-8 text with optional pretty-printing.
// WriteDocument is declared here but defined in :document (it needs XmlDocument
// complete). Ported from Sedulous.Xml/XmlWriter.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.xml:writer;

import draconic.foundation;
import :nodes;
import :escape;

using namespace draconic::foundation;

export namespace draconic::xml
{
    class XmlDocument; // defined in :document

    struct XmlWriteSettings
    {
        bool Indent = true;
        StringView IndentString = StringView(u8"\t");
        StringView NewLine = StringView(u8"\n");
        bool OmitDeclaration = false;
        bool CompactMode = false;

        [[nodiscard]] static XmlWriteSettings Default() { return XmlWriteSettings{}; }
    };

    class XmlWriter
    {
    public:
        XmlWriter() : m_output(&m_owned) {}
        explicit XmlWriter(String& output) : m_output(&output) {}
        explicit XmlWriter(XmlWriteSettings settings) : m_output(&m_owned), m_settings(settings) {}
        XmlWriter(String& output, XmlWriteSettings settings)
            : m_output(&output), m_settings(settings)
        {
        }

        [[nodiscard]] XmlWriteSettings Settings() const { return m_settings; }
        void SetSettings(XmlWriteSettings settings) { m_settings = settings; }

        void Clear()
        {
            m_output->Clear();
            m_indentLevel = 0;
        }
        [[nodiscard]] StringView Output() const { return *m_output; }
        void CopyTo(String& output) const { output.Append(*m_output); }

        // Defined in :document (needs XmlDocument complete).
        void WriteDocument(const XmlDocument& document);

        void WriteNode(const XmlNode& node)
        {
            switch (node.NodeType())
            {
            case XmlNodeType::Element:
                WriteElement(static_cast<const XmlElement&>(node));
                break;
            case XmlNodeType::Text:
                WriteText(static_cast<const XmlText&>(node));
                break;
            case XmlNodeType::CData:
                WriteCData(static_cast<const XmlCData&>(node));
                break;
            case XmlNodeType::Comment:
                WriteComment(static_cast<const XmlComment&>(node));
                break;
            case XmlNodeType::Declaration:
                WriteDeclaration(static_cast<const XmlDeclaration&>(node));
                break;
            case XmlNodeType::ProcessingInstruction:
                WriteProcessingInstruction(static_cast<const XmlProcessingInstruction&>(node));
                break;
            default:
                break; // Attribute / Document handled specially
            }
        }

        void WriteElement(const XmlElement& element)
        {
            WriteIndent();
            m_output->PushBack(u8'<');
            m_output->Append(element.TagName());
            for (XmlAttribute* attr : element.Attributes())
            {
                m_output->PushBack(u8' ');
                WriteAttribute(*attr);
            }

            if (!element.HasChildren())
            {
                m_output->Append(StringView(u8"/>"));
                return;
            }
            m_output->PushBack(u8'>');

            const bool simple = element.ChildCount() == 1 &&
                                (element.FirstChild()->NodeType() == XmlNodeType::Text ||
                                 element.FirstChild()->NodeType() == XmlNodeType::CData);

            if (!simple && !m_settings.CompactMode)
            {
                WriteNewLine();
            }
            ++m_indentLevel;

            for (XmlNode* child = element.FirstChild(); child != nullptr;
                 child = child->NextSibling())
            {
                if (simple || m_settings.CompactMode)
                {
                    if (child->NodeType() == XmlNodeType::Text)
                    {
                        EscapeText(static_cast<const XmlText*>(child)->Text(), *m_output);
                    }
                    else if (child->NodeType() == XmlNodeType::CData)
                    {
                        m_output->Append(StringView(u8"<![CDATA["));
                        m_output->Append(static_cast<const XmlCData*>(child)->Data());
                        m_output->Append(StringView(u8"]]>"));
                    }
                    else
                    {
                        WriteNode(*child);
                    }
                }
                else
                {
                    WriteNode(*child);
                    WriteNewLine();
                }
            }

            --m_indentLevel;
            if (!simple && !m_settings.CompactMode)
            {
                WriteIndent();
            }
            m_output->Append(StringView(u8"</"));
            m_output->Append(element.TagName());
            m_output->PushBack(u8'>');
        }

        void WriteAttribute(const XmlAttribute& attribute)
        {
            m_output->Append(attribute.Name());
            m_output->Append(StringView(u8"=\""));
            EscapeAttributeValue(attribute.Value(), *m_output);
            m_output->PushBack(u8'"');
        }

        void WriteText(const XmlText& text)
        {
            if (!m_settings.CompactMode)
            {
                WriteIndent();
            }
            EscapeText(text.Text(), *m_output);
        }

        void WriteCData(const XmlCData& cdata)
        {
            if (!m_settings.CompactMode)
            {
                WriteIndent();
            }
            m_output->Append(StringView(u8"<![CDATA["));
            m_output->Append(cdata.Data());
            m_output->Append(StringView(u8"]]>"));
        }

        void WriteComment(const XmlComment& comment)
        {
            if (!m_settings.CompactMode)
            {
                WriteIndent();
            }
            m_output->Append(StringView(u8"<!--"));
            m_output->Append(comment.Text());
            m_output->Append(StringView(u8"-->"));
        }

        void WriteDeclaration(const XmlDeclaration& declaration)
        {
            m_output->Append(StringView(u8"<?xml version=\""));
            m_output->Append(declaration.Version());
            m_output->Append(StringView(u8"\""));
            if (!declaration.Encoding().IsEmpty())
            {
                m_output->Append(StringView(u8" encoding=\""));
                m_output->Append(declaration.Encoding());
                m_output->Append(StringView(u8"\""));
            }
            if (!declaration.Standalone().IsEmpty())
            {
                m_output->Append(StringView(u8" standalone=\""));
                m_output->Append(declaration.Standalone());
                m_output->Append(StringView(u8"\""));
            }
            m_output->Append(StringView(u8"?>"));
        }

        void WriteProcessingInstruction(const XmlProcessingInstruction& pi)
        {
            if (!m_settings.CompactMode)
            {
                WriteIndent();
            }
            m_output->Append(StringView(u8"<?"));
            m_output->Append(pi.Target());
            if (!pi.Data().IsEmpty())
            {
                m_output->PushBack(u8' ');
                m_output->Append(pi.Data());
            }
            m_output->Append(StringView(u8"?>"));
        }

        // Escaping re-exposed for API parity (forwards to the :escape helpers).
        static void EscapeText(StringView text, String& output)
        {
            draconic::xml::EscapeText(text, output);
        }
        static void EscapeAttributeValue(StringView value, String& output)
        {
            draconic::xml::EscapeAttributeValue(value, output);
        }

    private:
        void WriteIndent()
        {
            if (m_settings.CompactMode || !m_settings.Indent)
            {
                return;
            }
            for (i32 i = 0; i < m_indentLevel; ++i)
            {
                m_output->Append(m_settings.IndentString);
            }
        }
        void WriteNewLine()
        {
            if (!m_settings.CompactMode)
            {
                m_output->Append(m_settings.NewLine);
            }
        }

        String m_owned;
        String* m_output;
        XmlWriteSettings m_settings = XmlWriteSettings::Default();
        i32 m_indentLevel = 0;
    };

    // Convenience: serialize an element to XML (no declaration).
    inline void ToXml(const XmlElement& element, String& output, bool compact = false)
    {
        XmlWriteSettings settings = XmlWriteSettings::Default();
        settings.CompactMode = compact;
        XmlWriter writer(output, settings);
        writer.WriteElement(element);
    }
}
