// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Xml - :nodes partition
//
// The DOM node hierarchy: XmlNode (intrusive tree) and its concrete kinds
// (Element, Attribute, Text, CData, Comment, Declaration, ProcessingInstruction).
// Nodes are heap objects; a node owns its children (its destructor deletes them
// recursively) and an element owns its attributes and local namespace map.

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"

export module foundation.xml:nodes;

import foundation.core;
import :lexer;
import :ns;
import :escape;

using namespace foundation::core;

export namespace foundation::xml
{
    enum class XmlNodeType : u8
    {
        Document,
        Element,
        Attribute,
        Text,
        CData,
        Comment,
        Declaration,
        ProcessingInstruction
    };

    class XmlDocument; // defined in :document; XmlNode::OwnerDocument resolved there
    struct ChildRange;

    // ----- XmlNode --------------------------------------------------------
    class XmlNode
    {
    public:
        virtual ~XmlNode()
        {
            XmlNode* child = m_firstChild;
            while (child != nullptr)
            {
                XmlNode* next = child->m_nextSibling;
                child->m_allocator->Delete(child);
                child = next;
            }
        }

        // The allocator this node was created from. The whole tree shares the
        // document's; detached subtrees keep their creator's.
        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }

        XmlNode(const XmlNode&) = delete;
        XmlNode& operator=(const XmlNode&) = delete;

        [[nodiscard]] XmlNodeType NodeType() const { return m_nodeType; }
        /// The line it began on in the text it was parsed from (1-based: an element's `<`, an
        /// attribute's name), so what reads a document can say where something is; 0 when it was
        /// made in code.
        [[nodiscard]] u32 Line() const noexcept { return m_line; }
        void SetLine(u32 line) noexcept { m_line = line; }
        [[nodiscard]] XmlNode* Parent() const { return m_parent; }
        [[nodiscard]] XmlNode* FirstChild() const { return m_firstChild; }
        [[nodiscard]] XmlNode* LastChild() const { return m_lastChild; }
        [[nodiscard]] XmlNode* PrevSibling() const { return m_prevSibling; }
        [[nodiscard]] XmlNode* NextSibling() const { return m_nextSibling; }
        [[nodiscard]] bool HasChildren() const { return m_firstChild != nullptr; }

        [[nodiscard]] usize ChildCount() const
        {
            usize count = 0;
            for (XmlNode* c = m_firstChild; c != nullptr; c = c->m_nextSibling)
            {
                ++count;
            }
            return count;
        }

        // Walks to the owning document (defined in :document).
        [[nodiscard]] XmlDocument* OwnerDocument() const;

        // Range over direct children (defined after ChildRange).
        [[nodiscard]] ChildRange Children() const;

        // --- tree manipulation ---
        virtual void AppendChild(XmlNode* child)
        {
            DIAGNOSTIC_ASSERT_MSG(child->m_parent == nullptr, "Node already has a parent");
            child->m_parent = this;
            child->m_prevSibling = m_lastChild;
            child->m_nextSibling = nullptr;
            if (m_lastChild != nullptr)
            {
                m_lastChild->m_nextSibling = child;
            }
            else
            {
                m_firstChild = child;
            }
            m_lastChild = child;
        }

        void PrependChild(XmlNode* child)
        {
            DIAGNOSTIC_ASSERT_MSG(child->m_parent == nullptr, "Node already has a parent");
            child->m_parent = this;
            child->m_prevSibling = nullptr;
            child->m_nextSibling = m_firstChild;
            if (m_firstChild != nullptr)
            {
                m_firstChild->m_prevSibling = child;
            }
            else
            {
                m_lastChild = child;
            }
            m_firstChild = child;
        }

        void InsertBefore(XmlNode* newChild, XmlNode* refChild)
        {
            if (refChild == nullptr)
            {
                AppendChild(newChild);
                return;
            }
            DIAGNOSTIC_ASSERT_MSG(refChild->m_parent == this,
                                "Reference child is not a child of this node");
            DIAGNOSTIC_ASSERT_MSG(newChild->m_parent == nullptr, "New node already has a parent");
            newChild->m_parent = this;
            newChild->m_prevSibling = refChild->m_prevSibling;
            newChild->m_nextSibling = refChild;
            if (refChild->m_prevSibling != nullptr)
            {
                refChild->m_prevSibling->m_nextSibling = newChild;
            }
            else
            {
                m_firstChild = newChild;
            }
            refChild->m_prevSibling = newChild;
        }

        void InsertAfter(XmlNode* newChild, XmlNode* refChild)
        {
            if (refChild == nullptr)
            {
                PrependChild(newChild);
                return;
            }
            DIAGNOSTIC_ASSERT_MSG(refChild->m_parent == this,
                                "Reference child is not a child of this node");
            DIAGNOSTIC_ASSERT_MSG(newChild->m_parent == nullptr, "New node already has a parent");
            newChild->m_parent = this;
            newChild->m_prevSibling = refChild;
            newChild->m_nextSibling = refChild->m_nextSibling;
            if (refChild->m_nextSibling != nullptr)
            {
                refChild->m_nextSibling->m_prevSibling = newChild;
            }
            else
            {
                m_lastChild = newChild;
            }
            refChild->m_nextSibling = newChild;
        }

        // Detaches a child (does not delete it).
        void RemoveChild(XmlNode* child)
        {
            DIAGNOSTIC_ASSERT_MSG(child->m_parent == this, "Node is not a child of this node");
            if (child->m_prevSibling != nullptr)
            {
                child->m_prevSibling->m_nextSibling = child->m_nextSibling;
            }
            else
            {
                m_firstChild = child->m_nextSibling;
            }
            if (child->m_nextSibling != nullptr)
            {
                child->m_nextSibling->m_prevSibling = child->m_prevSibling;
            }
            else
            {
                m_lastChild = child->m_prevSibling;
            }
            child->m_parent = nullptr;
            child->m_prevSibling = nullptr;
            child->m_nextSibling = nullptr;
        }

        void RemoveFromParent()
        {
            if (m_parent != nullptr)
            {
                m_parent->RemoveChild(this);
            }
        }

        // Detaches and deletes all children.
        void ClearChildren()
        {
            XmlNode* child = m_firstChild;
            while (child != nullptr)
            {
                XmlNode* next = child->m_nextSibling;
                child->m_parent = nullptr;
                child->m_prevSibling = nullptr;
                child->m_nextSibling = nullptr;
                child->m_allocator->Delete(child);
                child = next;
            }
            m_firstChild = nullptr;
            m_lastChild = nullptr;
        }

        // --- abstract ---
        virtual void GetInnerText(String& output) const = 0;
        virtual void GetOuterXml(String& output) const = 0;

    protected:
        XmlNode(IAllocator& allocator, XmlNodeType nodeType)
            : m_allocator(&allocator), m_nodeType(nodeType)
        {
        }

    private:
        IAllocator* m_allocator;
        XmlNodeType m_nodeType;
        u32 m_line = 0;
        XmlNode* m_parent = nullptr;
        XmlNode* m_firstChild = nullptr;
        XmlNode* m_lastChild = nullptr;
        XmlNode* m_prevSibling = nullptr;
        XmlNode* m_nextSibling = nullptr;
    };

    // Range-for over direct children, yielding XmlNode*.
    class ChildIterator
    {
    public:
        explicit ChildIterator(XmlNode* node) : m_node(node) {}
        [[nodiscard]] XmlNode* operator*() const { return m_node; }
        ChildIterator& operator++()
        {
            m_node = m_node->NextSibling();
            return *this;
        }
        [[nodiscard]] bool operator!=(const ChildIterator& other) const
        {
            return m_node != other.m_node;
        }

    private:
        XmlNode* m_node;
    };

    struct ChildRange
    {
        XmlNode* first;
        [[nodiscard]] ChildIterator begin() const { return ChildIterator(first); }
        [[nodiscard]] ChildIterator end() const { return ChildIterator(nullptr); }
    };

    inline ChildRange XmlNode::Children() const { return ChildRange{m_firstChild}; }

    // ----- XmlText --------------------------------------------------------
    class XmlText final : public XmlNode
    {
    public:
        explicit XmlText(IAllocator& allocator) : XmlNode(allocator, XmlNodeType::Text) {}
        XmlText(IAllocator& allocator, StringView text) : XmlNode(allocator, XmlNodeType::Text)
        {
            SetText(text);
        }

        [[nodiscard]] StringView Text() const { return m_text; }
        [[nodiscard]] bool IsWhitespace() const { return m_isWhitespace; }

        void SetText(StringView text)
        {
            m_text = String(text);
            UpdateWhitespaceFlag();
        }
        void AppendText(StringView text)
        {
            m_text.Append(text);
            UpdateWhitespaceFlag();
        }
        void Clear()
        {
            m_text.Clear();
            m_isWhitespace = true;
        }

        void GetInnerText(String& output) const override { output.Append(m_text); }
        void GetOuterXml(String& output) const override { EscapeText(m_text, output); }

    private:
        void UpdateWhitespaceFlag()
        {
            m_isWhitespace = true;
            for (usize i = 0; i < m_text.Size(); ++i)
            {
                if (!XmlLexer::IsWhitespace(m_text[i]))
                {
                    m_isWhitespace = false;
                    break;
                }
            }
        }
        String m_text;
        bool m_isWhitespace = true;
    };

    // ----- XmlCData -------------------------------------------------------
    class XmlCData final : public XmlNode
    {
    public:
        explicit XmlCData(IAllocator& allocator) : XmlNode(allocator, XmlNodeType::CData) {}
        XmlCData(IAllocator& allocator, StringView data) : XmlNode(allocator, XmlNodeType::CData)
        {
            m_data = String(data);
        }

        [[nodiscard]] StringView Data() const { return m_data; }
        void SetData(StringView data) { m_data = String(data); }
        void Clear() { m_data.Clear(); }

        void GetInnerText(String& output) const override { output.Append(m_data); }
        void GetOuterXml(String& output) const override
        {
            output.Append(StringView(u8"<![CDATA["));
            output.Append(m_data);
            output.Append(StringView(u8"]]>"));
        }

    private:
        String m_data;
    };

    // ----- XmlComment -----------------------------------------------------
    class XmlComment final : public XmlNode
    {
    public:
        explicit XmlComment(IAllocator& allocator) : XmlNode(allocator, XmlNodeType::Comment) {}
        XmlComment(IAllocator& allocator, StringView text)
            : XmlNode(allocator, XmlNodeType::Comment)
        {
            m_text = String(text);
        }

        [[nodiscard]] StringView Text() const { return m_text; }
        void SetText(StringView text) { m_text = String(text); }
        void Clear() { m_text.Clear(); }

        void GetInnerText(String&) const override {} // comments contribute no text
        void GetOuterXml(String& output) const override
        {
            output.Append(StringView(u8"<!--"));
            output.Append(m_text);
            output.Append(StringView(u8"-->"));
        }

    private:
        String m_text;
    };

    // ----- XmlDeclaration -------------------------------------------------
    class XmlDeclaration final : public XmlNode
    {
    public:
        explicit XmlDeclaration(IAllocator& allocator)
            : XmlNode(allocator, XmlNodeType::Declaration)
        {
        }
        XmlDeclaration(IAllocator& allocator, StringView version, StringView encoding,
                       StringView standalone)
            : XmlNode(allocator, XmlNodeType::Declaration), m_version(version),
              m_encoding(encoding),
              m_standalone(standalone)
        {
        }

        [[nodiscard]] StringView Version() const { return m_version; }
        [[nodiscard]] StringView Encoding() const { return m_encoding; }
        [[nodiscard]] StringView Standalone() const { return m_standalone; }
        void SetVersion(StringView v) { m_version = String(v); }
        void SetEncoding(StringView e) { m_encoding = String(e); }
        void SetStandalone(StringView s) { m_standalone = String(s); }

        void GetInnerText(String&) const override {}
        void GetOuterXml(String& output) const override
        {
            output.Append(StringView(u8"<?xml version=\""));
            output.Append(m_version);
            output.Append(StringView(u8"\""));
            if (!m_encoding.IsEmpty())
            {
                output.Append(StringView(u8" encoding=\""));
                output.Append(m_encoding);
                output.Append(StringView(u8"\""));
            }
            if (!m_standalone.IsEmpty())
            {
                output.Append(StringView(u8" standalone=\""));
                output.Append(m_standalone);
                output.Append(StringView(u8"\""));
            }
            output.Append(StringView(u8"?>"));
        }

    private:
        String m_version = String(u8"1.0");
        String m_encoding = String(u8"utf-8");
        String m_standalone;
    };

    // ----- XmlProcessingInstruction ---------------------------------------
    class XmlProcessingInstruction final : public XmlNode
    {
    public:
        explicit XmlProcessingInstruction(IAllocator& allocator)
            : XmlNode(allocator, XmlNodeType::ProcessingInstruction)
        {
        }
        XmlProcessingInstruction(IAllocator& allocator, StringView target, StringView data)
            : XmlNode(allocator, XmlNodeType::ProcessingInstruction), m_target(target),
              m_data(data)
        {
        }

        [[nodiscard]] StringView Target() const { return m_target; }
        [[nodiscard]] StringView Data() const { return m_data; }
        void SetTarget(StringView t) { m_target = String(t); }
        void SetData(StringView d) { m_data = String(d); }

        void GetInnerText(String&) const override {}
        void GetOuterXml(String& output) const override
        {
            output.Append(StringView(u8"<?"));
            output.Append(m_target);
            if (!m_data.IsEmpty())
            {
                output.PushBack(u8' ');
                output.Append(m_data);
            }
            output.Append(StringView(u8"?>"));
        }

    private:
        String m_target;
        String m_data;
    };

    // ----- XmlAttribute ---------------------------------------------------
    class XmlElement; // forward (OwnerElement)

    class XmlAttribute final : public XmlNode
    {
    public:
        explicit XmlAttribute(IAllocator& allocator) : XmlNode(allocator, XmlNodeType::Attribute)
        {
        }
        XmlAttribute(IAllocator& allocator, StringView name, StringView value)
            : XmlNode(allocator, XmlNodeType::Attribute)
        {
            SetName(name);
            m_value = String(value);
        }
        XmlAttribute(IAllocator& allocator, StringView prefix, StringView localName,
                     StringView namespaceUri,
                     StringView value)
            : XmlNode(allocator, XmlNodeType::Attribute)
        {
            SetQualifiedName(prefix, localName, namespaceUri);
            m_value = String(value);
        }

        [[nodiscard]] StringView Name() const { return m_name; }
        [[nodiscard]] StringView Prefix() const { return m_prefix; }
        [[nodiscard]] StringView LocalName() const { return m_localName; }
        [[nodiscard]] StringView NamespaceUri() const { return m_namespaceUri; }
        [[nodiscard]] StringView Value() const { return m_value; }
        [[nodiscard]] XmlElement* OwnerElement() const { return m_ownerElement; }

        void SetName(StringView name)
        {
            m_name = String(name);
            XmlLexer::SplitQualifiedName(name, m_prefix, m_localName);
        }

        void SetQualifiedName(StringView prefix, StringView localName, StringView namespaceUri)
        {
            m_prefix = String(prefix);
            m_localName = String(localName);
            m_namespaceUri = String(namespaceUri);
            m_name.Clear();
            if (!prefix.IsEmpty())
            {
                m_name.Append(prefix);
                m_name.PushBack(u8':');
            }
            m_name.Append(localName);
        }

        void SetValue(StringView value) { m_value = String(value); }
        void SetOwnerElement(XmlElement* element) { m_ownerElement = element; }

        [[nodiscard]] bool IsNamespaceDeclaration() const
        {
            return m_name == StringView(u8"xmlns") || m_prefix == StringView(u8"xmlns");
        }
        [[nodiscard]] StringView DeclaredPrefix() const
        {
            if (m_name == StringView(u8"xmlns"))
            {
                return StringView(u8"");
            }
            if (m_prefix == StringView(u8"xmlns"))
            {
                return m_localName;
            }
            return StringView(u8"");
        }
        [[nodiscard]] StringView DeclaredNamespaceUri() const
        {
            return IsNamespaceDeclaration() ? StringView(m_value) : StringView(u8"");
        }

        void GetInnerText(String& output) const override { output.Append(m_value); }
        void GetOuterXml(String& output) const override
        {
            output.Append(m_name);
            output.Append(StringView(u8"=\""));
            EscapeAttributeValue(m_value, output);
            output.PushBack(u8'"');
        }

    private:
        String m_name;
        String m_prefix;
        String m_localName;
        String m_namespaceUri;
        String m_value;
        XmlElement* m_ownerElement = nullptr;
    };

    // ----- XmlElement -----------------------------------------------------
    class XmlElement final : public XmlNode
    {
    public:
        explicit XmlElement(IAllocator& allocator) : XmlNode(allocator, XmlNodeType::Element) {}
        XmlElement(IAllocator& allocator, StringView tagName)
            : XmlNode(allocator, XmlNodeType::Element)
        {
            SetTagName(tagName);
        }
        XmlElement(IAllocator& allocator, StringView prefix, StringView localName,
                   StringView namespaceUri)
            : XmlNode(allocator, XmlNodeType::Element)
        {
            SetQualifiedName(prefix, localName, namespaceUri);
        }

        ~XmlElement() override
        {
            for (XmlAttribute* attr : m_attributes)
            {
                attr->Allocator().Delete(attr);
            }
        }

        [[nodiscard]] StringView TagName() const { return m_tagName; }
        [[nodiscard]] StringView Prefix() const { return m_prefix; }
        [[nodiscard]] StringView LocalName() const { return m_localName; }
        [[nodiscard]] StringView NamespaceUri() const { return m_namespaceUri; }

        void SetTagName(StringView name)
        {
            m_tagName = String(name);
            XmlLexer::SplitQualifiedName(name, m_prefix, m_localName);
        }
        void SetQualifiedName(StringView prefix, StringView localName, StringView namespaceUri)
        {
            m_prefix = String(prefix);
            m_localName = String(localName);
            m_namespaceUri = String(namespaceUri);
            m_tagName.Clear();
            if (!prefix.IsEmpty())
            {
                m_tagName.Append(prefix);
                m_tagName.PushBack(u8':');
            }
            m_tagName.Append(localName);
        }

        // --- attributes ---
        [[nodiscard]] usize AttributeCount() const { return m_attributes.Size(); }
        [[nodiscard]] const Array<XmlAttribute*>& Attributes() const { return m_attributes; }

        [[nodiscard]] bool HasAttribute(StringView name) const
        {
            for (XmlAttribute* a : m_attributes)
            {
                if (a->Name() == name)
                {
                    return true;
                }
            }
            return false;
        }
        [[nodiscard]] bool HasAttributeNS(StringView nsUri, StringView localName) const
        {
            for (XmlAttribute* a : m_attributes)
            {
                if (a->NamespaceUri() == nsUri && a->LocalName() == localName)
                {
                    return true;
                }
            }
            return false;
        }
        [[nodiscard]] StringView GetAttribute(StringView name) const
        {
            for (XmlAttribute* a : m_attributes)
            {
                if (a->Name() == name)
                {
                    return a->Value();
                }
            }
            return StringView(u8"");
        }
        [[nodiscard]] StringView GetAttributeNS(StringView nsUri, StringView localName) const
        {
            for (XmlAttribute* a : m_attributes)
            {
                if (a->NamespaceUri() == nsUri && a->LocalName() == localName)
                {
                    return a->Value();
                }
            }
            return StringView(u8"");
        }
        [[nodiscard]] XmlAttribute* GetAttributeNode(StringView name) const
        {
            for (XmlAttribute* a : m_attributes)
            {
                if (a->Name() == name)
                {
                    return a;
                }
            }
            return nullptr;
        }
        [[nodiscard]] XmlAttribute* GetAttributeNodeNS(StringView nsUri, StringView localName) const
        {
            for (XmlAttribute* a : m_attributes)
            {
                if (a->NamespaceUri() == nsUri && a->LocalName() == localName)
                {
                    return a;
                }
            }
            return nullptr;
        }

        void SetAttribute(StringView name, StringView value)
        {
            for (XmlAttribute* a : m_attributes)
            {
                if (a->Name() == name)
                {
                    a->SetValue(value);
                    return;
                }
            }
            XmlAttribute* attr = Allocator().New<XmlAttribute>(Allocator(), name, value);
            attr->SetOwnerElement(this);
            m_attributes.PushBack(attr);
            if (attr->IsNamespaceDeclaration())
            {
                DeclareNamespace(attr->DeclaredPrefix(), attr->DeclaredNamespaceUri());
            }
        }
        void SetAttributeNS(StringView namespaceUri, StringView qualifiedName, StringView value)
        {
            String prefix, localName;
            XmlLexer::SplitQualifiedName(qualifiedName, prefix, localName);
            for (XmlAttribute* a : m_attributes)
            {
                if (a->NamespaceUri() == namespaceUri && a->LocalName() == StringView(localName))
                {
                    a->SetValue(value);
                    return;
                }
            }
            XmlAttribute* attr = Allocator().New<XmlAttribute>(
                Allocator(),
                StringView(prefix), StringView(localName), namespaceUri, value);
            attr->SetOwnerElement(this);
            m_attributes.PushBack(attr);
        }
        void SetAttributeNode(XmlAttribute* attr)
        {
            for (usize i = 0; i < m_attributes.Size(); ++i)
            {
                if (m_attributes[i]->Name() == attr->Name())
                {
                    m_attributes[i]->SetOwnerElement(nullptr);
                    m_attributes[i]->Allocator().Delete(m_attributes[i]);
                    m_attributes.RemoveAt(i);
                    break;
                }
            }
            attr->SetOwnerElement(this);
            m_attributes.PushBack(attr);
            if (attr->IsNamespaceDeclaration())
            {
                DeclareNamespace(attr->DeclaredPrefix(), attr->DeclaredNamespaceUri());
            }
        }
        void RemoveAttribute(StringView name)
        {
            for (usize i = 0; i < m_attributes.Size(); ++i)
            {
                if (m_attributes[i]->Name() == name)
                {
                    m_attributes[i]->SetOwnerElement(nullptr);
                    m_attributes[i]->Allocator().Delete(m_attributes[i]);
                    m_attributes.RemoveAt(i);
                    return;
                }
            }
        }
        void RemoveAttributeNS(StringView nsUri, StringView localName)
        {
            for (usize i = 0; i < m_attributes.Size(); ++i)
            {
                if (m_attributes[i]->NamespaceUri() == nsUri &&
                    m_attributes[i]->LocalName() == localName)
                {
                    m_attributes[i]->SetOwnerElement(nullptr);
                    m_attributes[i]->Allocator().Delete(m_attributes[i]);
                    m_attributes.RemoveAt(i);
                    return;
                }
            }
        }
        // Detaches (does not delete) the attribute node.
        void RemoveAttributeNode(XmlAttribute* attr)
        {
            for (usize i = 0; i < m_attributes.Size(); ++i)
            {
                if (m_attributes[i] == attr)
                {
                    attr->SetOwnerElement(nullptr);
                    m_attributes.RemoveAt(i);
                    return;
                }
            }
        }
        void ClearAttributes()
        {
            for (XmlAttribute* a : m_attributes)
            {
                a->SetOwnerElement(nullptr);
                a->Allocator().Delete(a);
            }
            m_attributes.Clear();
            m_localNamespaces.Clear();
        }

        // --- namespaces ---
        void DeclareNamespace(StringView prefix, StringView uri)
        {
            m_localNamespaces.InsertOrAssign(String(prefix), String(uri));
        }

        [[nodiscard]] StringView ResolveNamespacePrefix(StringView prefix) const
        {
            if (const String* uri = m_localNamespaces.Find(String(prefix)))
            {
                return *uri;
            }
            if (XmlNode* p = Parent(); p != nullptr && p->NodeType() == XmlNodeType::Element)
            {
                return static_cast<const XmlElement*>(p)->ResolveNamespacePrefix(prefix);
            }
            if (prefix == StringView(u8"xml"))
            {
                return XmlNamespaces::Xml;
            }
            if (prefix == StringView(u8"xmlns"))
            {
                return XmlNamespaces::Xmlns;
            }
            return StringView(u8"");
        }
        [[nodiscard]] StringView ResolveNamespaceUri(StringView uri) const
        {
            for (const auto& entry : m_localNamespaces)
            {
                if (StringView(entry.value) == uri)
                {
                    return entry.key;
                }
            }
            if (XmlNode* p = Parent(); p != nullptr && p->NodeType() == XmlNodeType::Element)
            {
                return static_cast<const XmlElement*>(p)->ResolveNamespaceUri(uri);
            }
            if (uri == XmlNamespaces::Xml)
            {
                return StringView(u8"xml");
            }
            if (uri == XmlNamespaces::Xmlns)
            {
                return StringView(u8"xmlns");
            }
            return StringView(u8"");
        }

        // --- child element navigation (skips non-elements) ---
        [[nodiscard]] XmlElement* FirstChildElement() const
        {
            return NextElement(FirstChild(), true);
        }
        [[nodiscard]] XmlElement* LastChildElement() const
        {
            for (XmlNode* c = LastChild(); c != nullptr; c = c->PrevSibling())
            {
                if (c->NodeType() == XmlNodeType::Element)
                {
                    return static_cast<XmlElement*>(c);
                }
            }
            return nullptr;
        }
        [[nodiscard]] XmlElement* NextSiblingElement() const
        {
            return NextElement(NextSibling(), true);
        }
        [[nodiscard]] XmlElement* PrevSiblingElement() const
        {
            for (XmlNode* s = PrevSibling(); s != nullptr; s = s->PrevSibling())
            {
                if (s->NodeType() == XmlNodeType::Element)
                {
                    return static_cast<XmlElement*>(s);
                }
            }
            return nullptr;
        }
        [[nodiscard]] XmlElement* GetFirstChildElement(StringView tagName) const
        {
            for (XmlNode* c = FirstChild(); c != nullptr; c = c->NextSibling())
            {
                if (c->NodeType() == XmlNodeType::Element)
                {
                    XmlElement* e = static_cast<XmlElement*>(c);
                    if (e->TagName() == tagName)
                    {
                        return e;
                    }
                }
            }
            return nullptr;
        }
        void GetChildElements(StringView tagName, Array<XmlElement*>& results) const
        {
            for (XmlNode* c = FirstChild(); c != nullptr; c = c->NextSibling())
            {
                if (c->NodeType() == XmlNodeType::Element)
                {
                    XmlElement* e = static_cast<XmlElement*>(c);
                    if (tagName.IsEmpty() || e->TagName() == tagName)
                    {
                        results.PushBack(e);
                    }
                }
            }
        }
        void GetDescendantElements(StringView tagName, Array<XmlElement*>& results) const
        {
            for (XmlNode* c = FirstChild(); c != nullptr; c = c->NextSibling())
            {
                if (c->NodeType() == XmlNodeType::Element)
                {
                    XmlElement* e = static_cast<XmlElement*>(c);
                    if (tagName.IsEmpty() || e->TagName() == tagName)
                    {
                        results.PushBack(e);
                    }
                    e->GetDescendantElements(tagName, results);
                }
            }
        }

        // --- text content ---
        void GetTextContent(String& output) const { GetInnerText(output); }
        void SetTextContent(StringView text)
        {
            ClearChildren();
            if (!text.IsEmpty())
            {
                AppendChild(Allocator().New<XmlText>(Allocator(), text));
            }
        }

        void GetInnerText(String& output) const override
        {
            for (XmlNode* c = FirstChild(); c != nullptr; c = c->NextSibling())
            {
                c->GetInnerText(output);
            }
        }
        void GetOuterXml(String& output) const override
        {
            output.PushBack(u8'<');
            output.Append(m_tagName);
            for (XmlAttribute* a : m_attributes)
            {
                output.PushBack(u8' ');
                a->GetOuterXml(output);
            }
            if (!HasChildren())
            {
                output.Append(StringView(u8"/>"));
                return;
            }
            output.PushBack(u8'>');
            for (XmlNode* c = FirstChild(); c != nullptr; c = c->NextSibling())
            {
                c->GetOuterXml(output);
            }
            output.Append(StringView(u8"</"));
            output.Append(m_tagName);
            output.PushBack(u8'>');
        }

    private:
        static XmlElement* NextElement(XmlNode* start, bool forward)
        {
            for (XmlNode* c = start; c != nullptr;
                 c = forward ? c->NextSibling() : c->PrevSibling())
            {
                if (c->NodeType() == XmlNodeType::Element)
                {
                    return static_cast<XmlElement*>(c);
                }
            }
            return nullptr;
        }

        String m_tagName;
        String m_prefix;
        String m_localName;
        String m_namespaceUri;
        Array<XmlAttribute*> m_attributes;
        HashMap<String, String> m_localNamespaces;
    };
}
