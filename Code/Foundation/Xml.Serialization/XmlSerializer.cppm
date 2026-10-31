// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Xml.Serialization - the `foundation.xml.serialization` module.
//
// An XML backend for Core's format-agnostic ISerializer/Serializer contract. One
// Serialize() path runs either direction; this maps the Key()/scope/Scalar/Text calls
// onto an XML DOM (foundation.xml) with typed element tags and a `name` attribute.
//
// Layering: kept out of Core (which can't depend on Xml) and out of the XML DOM
// library (which stays serialization-agnostic).

module;
#include "Core/Prelude.h"
#include <cstdlib> // strtoll, strtoull, strtod

export module foundation.xml.serialization;

import foundation.core;
import foundation.xml;

using namespace foundation::core;

export namespace foundation::xml
{
    // ISerializer backend over an XML DOM.
    //
    // Layout: each field is a child element. Its tag names the scalar kind
    // ("i32", "f32", "object", "array", ...); a keyed field carries a `name`
    // attribute, array elements are positional (unnamed). Scalars and strings store
    // their value as element text; arrays carry a `count` attribute.
    class XmlSerializer final : public Serializer
    {
    public:
        // Write mode: builds a fresh document rooted at <root>; the allocator
        // (required: the owner decides) backs the whole node tree.
        explicit XmlSerializer(IAllocator& allocator)
            : Serializer(SerializeMode::Write), m_doc(allocator)
        {
            XmlElement* root = m_doc.CreateElement(u8"root");
            m_doc.AppendChild(root);
            m_writeCurrent = root;
        }

        // Read mode: reads from an already-parsed document (caller owns it).
        explicit XmlSerializer(XmlDocument& source)
            : Serializer(SerializeMode::Read), m_doc(source.Allocator())
        {
            m_readStack.PushBack(ReadScope{source.RootElement(), nullptr});
            ResetCursor(m_readStack[0]);
        }

        // Write-mode output (declaration omitted).
        void GetOutput(String& output) const
        {
            XmlWriteSettings settings = XmlWriteSettings::Default();
            settings.OmitDeclaration = true;
            m_doc.WriteTo(output, settings);
        }
        void GetOutput(String& output, XmlWriteSettings settings) const
        {
            m_doc.WriteTo(output, settings);
        }

        // --- ISerializer ---------------------------------------------------
        // Self-describing (typed element tags + name attributes), so consumers can skip an explicit
        // format-version field and stay readable across format changes.
        [[nodiscard]] bool IsSelfDescribing() const noexcept override { return true; }

        void Key(const char* name) noexcept override { m_pendingKey = name; }

        // Whether the current scope still has an element under `name`, searched forward from the
        // cursor as a keyed read searches, and not consumed. Writing, every key is there.
        [[nodiscard]] bool HasKey(const char* name) const override
        {
            if (IsWriting())
            {
                return true;
            }
            if (m_readStack.IsEmpty())
            {
                return false;
            }
            const StringView wanted(reinterpret_cast<const utf8char*>(name));
            for (XmlNode* c = m_readStack[m_readStack.Size() - 1].cursor; c != nullptr; c = c->NextSibling())
            {
                if (c->NodeType() == XmlNodeType::Element &&
                    static_cast<XmlElement*>(c)->GetAttribute(u8"name") == wanted)
                {
                    return true;
                }
            }
            return false;
        }

        void BeginObject() override
        {
            if (IsWriting())
            {
                XmlElement* element = MakeElement(u8"object");
                m_writeStack.PushBack(m_writeCurrent);
                m_writeCurrent = element;
            }
            else
            {
                XmlElement* element = Locate();
                if (element == nullptr)
                {
                    FailRead(ErrorCode::NotFound, u8"no object");
                    return;
                }
                PushRead(element);
            }
        }

        void EndObject() override
        {
            if (IsWriting())
            {
                if (m_writeStack.Size() == 0)
                {
                    Fail(ErrorCode::Internal);
                    return;
                }
                m_writeCurrent = m_writeStack[m_writeStack.Size() - 1];
                m_writeStack.PopBack();
            }
            else
            {
                if (m_readStack.Size() <= 1)
                {
                    Fail(ErrorCode::Internal);
                    return;
                }
                m_readStack.PopBack();
            }
        }

        void BeginArray(u32& count) override
        {
            if (IsWriting())
            {
                XmlElement* element = MakeElement(u8"array");
                String countStr;
                AppendValue(countStr, count);
                element->SetAttribute(u8"count", countStr);
                m_writeStack.PushBack(m_writeCurrent);
                m_writeCurrent = element;
            }
            else
            {
                XmlElement* element = Locate();
                if (element == nullptr)
                {
                    count = 0;
                    FailRead(ErrorCode::NotFound, u8"no array");
                    return;
                }
                u64 parsed = 0;
                if (!ParseU64(element->GetAttribute(u8"count"), parsed))
                {
                    FailRead(ErrorCode::Internal, u8"no count on the array");
                }
                count = static_cast<u32>(parsed);
                PushRead(element);
            }
        }

        void EndArray() override { EndObject(); }

        void Scalar(void* value, ScalarKind kind) override
        {
            if (IsWriting())
            {
                XmlElement* element = MakeElement(ScalarTag(kind));
                String text;
                WriteScalar(value, kind, text);
                element->SetTextContent(text);
            }
            else
            {
                XmlElement* element = Locate();
                if (element == nullptr)
                {
                    FailRead(ErrorCode::NotFound, Format(u8"no {}", ScalarTag(kind)).AsView());
                    return;
                }
                String text;
                element->GetTextContent(text);
                if (!ReadScalar(value, kind, text))
                {
                    FailRead(ErrorCode::Internal,
                             Format(u8"not a {}: <{}>", ScalarTag(kind), element->TagName()).AsView());
                }
            }
        }

        void Text(String& value) override
        {
            if (IsWriting())
            {
                XmlElement* element = MakeElement(u8"string");
                element->SetTextContent(value);
            }
            else
            {
                XmlElement* element = Locate();
                if (element == nullptr)
                {
                    FailRead(ErrorCode::NotFound, u8"no string");
                    return;
                }
                value.Clear();
                element->GetTextContent(value);
            }
        }

        void Blob(void* data, usize size) override
        {
            if (IsWriting())
            {
                XmlElement* element = MakeElement(u8"blob");
                String hex;
                EncodeHex(static_cast<const u8*>(data), size, hex);
                element->SetTextContent(hex);
            }
            else
            {
                XmlElement* element = Locate();
                if (element == nullptr)
                {
                    FailRead(ErrorCode::NotFound, u8"no blob");
                    return;
                }
                String hex;
                element->GetTextContent(hex);
                if (!DecodeHex(hex, static_cast<u8*>(data), size))
                {
                    Fail(ErrorCode::Internal);
                }
            }
        }

        // Unknown-section passthrough. XML is self-describing, so BeginFramedRegion/EndFramedRegion are
        // the inherited no-ops (element boundaries delimit the region); this captures (read) or
        // re-injects (write) the current scope's remaining element children verbatim, so a section
        // whose type this build cannot instantiate survives a load/save round-trip.
        bool RawRemainder(Array<u8>& blob) override
        {
            if (IsWriting())
            {
                // Parse the captured fragment (wrapped in a synthetic root) and move its element children
                // into the current write element. The scratch document uses m_doc's allocator, so
                // reparenting transfers ownership cleanly (RemoveChild detaches without delete; AppendChild
                // adopts, and scratch's destructor only deletes what is still attached).
                if (blob.IsEmpty())
                {
                    return true;
                }
                String wrapped;
                wrapped += StringView(u8"<__frame__>");
                wrapped +=
                    StringView(reinterpret_cast<const utf8char*>(blob.Data()), blob.Size());
                wrapped += StringView(u8"</__frame__>");
                XmlDocument scratch(m_doc.Allocator());
                if (scratch.Parse(wrapped.AsView()) != XmlResult::Ok)
                {
                    Fail(ErrorCode::Internal);
                    return false;
                }
                XmlElement* root = scratch.RootElement();
                XmlNode* child = (root != nullptr) ? root->FirstChild() : nullptr;
                while (child != nullptr)
                {
                    XmlNode* next = child->NextSibling();
                    if (child->NodeType() == XmlNodeType::Element)
                    {
                        root->RemoveChild(child);
                        m_writeCurrent->AppendChild(child);
                    }
                    child = next;
                }
                return true;
            }

            // Read: serialize the current scope's remaining element children to bytes, then consume them.
            if (m_readStack.IsEmpty())
            {
                return false;
            }
            ReadScope& scope = m_readStack[m_readStack.Size() - 1];
            String captured;
            for (XmlNode* c = scope.cursor; c != nullptr; c = c->NextSibling())
            {
                if (c->NodeType() == XmlNodeType::Element)
                {
                    String outer;
                    c->GetOuterXml(outer);
                    captured += outer.AsView();
                }
            }
            scope.cursor = nullptr; // fully consumed
            const StringView sv = captured.AsView();
            blob.Clear();
            blob.Resize(sv.Size());
            if (sv.Size() > 0)
            {
                MemCopy(blob.Data(), sv.Data(), sv.Size());
            }
            return true;
        }

        void DescribeFailure(String& outText) const override { outText += m_failure.AsView(); }

    private:
        struct ReadScope
        {
            XmlElement* element;
            XmlNode* cursor;
        };

        // --- write helpers ---
        XmlElement* MakeElement(StringView tag)
        {
            XmlElement* element = m_doc.CreateElement(tag);
            if (m_pendingKey != nullptr)
            {
                element->SetAttribute(u8"name",
                                      StringView(reinterpret_cast<const utf8char*>(m_pendingKey)));
                m_pendingKey = nullptr;
            }
            m_writeCurrent->AppendChild(element);
            return element;
        }

        // --- read helpers ---

        // Fails the read, and the first failure also says where: what was wanted (by key when
        // one was asked for), the scopes it was wanted in, and what stood there instead.
        void FailRead(ErrorCode code, StringView what)
        {
            if (IsOk())
            {
                m_failure = String(what);
                if (m_lastKey != nullptr)
                {
                    m_failure += Format(u8" '{}'", StringView(reinterpret_cast<const utf8char*>(m_lastKey)))
                                     .AsView();
                }
                m_failure += StringView(u8" at ");
                bool any = false;
                for (usize i = 1; i < m_readStack.Size(); ++i)
                {
                    const XmlElement* element = m_readStack[i].element;
                    if (element == nullptr)
                    {
                        continue;
                    }
                    if (any)
                    {
                        m_failure += StringView(u8"/");
                    }
                    const StringView name = element->GetAttribute(u8"name");
                    m_failure += name.IsEmpty() ? Format(u8"<{}>", element->TagName()).AsView() : name;
                    any = true;
                }
                if (!any)
                {
                    m_failure += StringView(u8"the top");
                }
                if (!m_readStack.IsEmpty())
                {
                    XmlNode* node = m_readStack[m_readStack.Size() - 1].cursor;
                    while (node != nullptr && node->NodeType() != XmlNodeType::Element)
                    {
                        node = node->NextSibling();
                    }
                    if (node != nullptr)
                    {
                        const XmlElement* next = static_cast<const XmlElement*>(node);
                        const StringView nextName = next->GetAttribute(u8"name");
                        m_failure += StringView(u8" (next there: <");
                        m_failure += next->TagName();
                        if (!nextName.IsEmpty())
                        {
                            m_failure += Format(u8" name=\"{}\"", nextName).AsView();
                        }
                        m_failure += StringView(u8">)");
                    }
                    else
                    {
                        m_failure += StringView(u8" (nothing left there)");
                    }
                }
            }
            Fail(code);
        }
        static void ResetCursor(ReadScope& scope)
        {
            scope.cursor = (scope.element != nullptr) ? scope.element->FirstChild() : nullptr;
        }
        void PushRead(XmlElement* element)
        {
            ReadScope scope{element, nullptr};
            ResetCursor(scope);
            m_readStack.PushBack(scope);
        }

        // Locate the source element for the next read: by `name` if a key is
        // pending, else the next positional (unnamed) child element.
        XmlElement* Locate()
        {
            ReadScope& scope = m_readStack[m_readStack.Size() - 1];
            if (scope.element == nullptr)
            {
                m_pendingKey = nullptr;
                return nullptr;
            }

            m_lastKey = m_pendingKey;
            if (m_pendingKey != nullptr)
            {
                const StringView name(reinterpret_cast<const utf8char*>(m_pendingKey));
                m_pendingKey = nullptr;
                // Search forward from the scope cursor, consuming on a hit. Writes emit
                // fields sequentially, so reads are sequential too; starting at FirstChild
                // every time would make an array of keyed structs (repeated flat key
                // groups, such as a model manifest's nodes) read every element as a copy of
                // the first group. A miss leaves the cursor untouched (the caller fails
                // loudly; the scope stays consumable).
                for (XmlNode* c = scope.cursor; c != nullptr; c = c->NextSibling())
                {
                    if (c->NodeType() == XmlNodeType::Element)
                    {
                        XmlElement* e = static_cast<XmlElement*>(c);
                        if (e->GetAttribute(u8"name") == name)
                        {
                            scope.cursor = c->NextSibling();
                            return e;
                        }
                    }
                }
                return nullptr;
            }

            // Positional: advance the cursor to the next element child.
            while (scope.cursor != nullptr && scope.cursor->NodeType() != XmlNodeType::Element)
            {
                scope.cursor = scope.cursor->NextSibling();
            }
            if (scope.cursor == nullptr)
            {
                return nullptr;
            }
            XmlElement* element = static_cast<XmlElement*>(scope.cursor);
            scope.cursor = scope.cursor->NextSibling();
            return element;
        }

        // --- scalar text <-> value ---
        [[nodiscard]] static StringView ScalarTag(ScalarKind kind)
        {
            switch (kind)
            {
            case ScalarKind::Bool:
                return StringView(u8"bool");
            case ScalarKind::Int8:
                return StringView(u8"i8");
            case ScalarKind::UInt8:
                return StringView(u8"u8");
            case ScalarKind::Int16:
                return StringView(u8"i16");
            case ScalarKind::UInt16:
                return StringView(u8"u16");
            case ScalarKind::Int32:
                return StringView(u8"i32");
            case ScalarKind::UInt32:
                return StringView(u8"u32");
            case ScalarKind::Int64:
                return StringView(u8"i64");
            case ScalarKind::UInt64:
                return StringView(u8"u64");
            case ScalarKind::Float32:
                return StringView(u8"f32");
            case ScalarKind::Float64:
                return StringView(u8"f64");
            }
            return StringView(u8"scalar");
        }

        static void WriteScalar(const void* p, ScalarKind kind, String& out)
        {
            switch (kind)
            {
            case ScalarKind::Bool:
                out.Append(*static_cast<const bool*>(p) ? StringView(u8"true")
                                                        : StringView(u8"false"));
                break;
            case ScalarKind::Int8:
                AppendValue(out, static_cast<i64>(*static_cast<const i8*>(p)));
                break;
            case ScalarKind::UInt8:
                AppendValue(out, static_cast<u64>(*static_cast<const u8*>(p)));
                break;
            case ScalarKind::Int16:
                AppendValue(out, static_cast<i64>(*static_cast<const i16*>(p)));
                break;
            case ScalarKind::UInt16:
                AppendValue(out, static_cast<u64>(*static_cast<const u16*>(p)));
                break;
            case ScalarKind::Int32:
                AppendValue(out, *static_cast<const i32*>(p));
                break;
            case ScalarKind::UInt32:
                AppendValue(out, *static_cast<const u32*>(p));
                break;
            case ScalarKind::Int64:
                AppendValue(out, *static_cast<const i64*>(p));
                break;
            case ScalarKind::UInt64:
                AppendValue(out, *static_cast<const u64*>(p));
                break;
            case ScalarKind::Float32:
                AppendValue(out, *static_cast<const f32*>(p));
                break;
            case ScalarKind::Float64:
                AppendValue(out, *static_cast<const f64*>(p));
                break;
            }
        }

        static bool ReadScalar(void* p, ScalarKind kind, const String& text)
        {
            const char* s = reinterpret_cast<const char*>(text.CStr());
            switch (kind)
            {
            case ScalarKind::Bool:
            {
                if (text == StringView(u8"true") || text == StringView(u8"1"))
                {
                    *static_cast<bool*>(p) = true;
                    return true;
                }
                if (text == StringView(u8"false") || text == StringView(u8"0"))
                {
                    *static_cast<bool*>(p) = false;
                    return true;
                }
                return false;
            }
            case ScalarKind::Int8:
            {
                i64 v = 0;
                if (!ParseI64(s, v))
                    return false;
                *static_cast<i8*>(p) = static_cast<i8>(v);
                return true;
            }
            case ScalarKind::Int16:
            {
                i64 v = 0;
                if (!ParseI64(s, v))
                    return false;
                *static_cast<i16*>(p) = static_cast<i16>(v);
                return true;
            }
            case ScalarKind::Int32:
            {
                i64 v = 0;
                if (!ParseI64(s, v))
                    return false;
                *static_cast<i32*>(p) = static_cast<i32>(v);
                return true;
            }
            case ScalarKind::Int64:
            {
                i64 v = 0;
                if (!ParseI64(s, v))
                    return false;
                *static_cast<i64*>(p) = v;
                return true;
            }
            case ScalarKind::UInt8:
            {
                u64 v = 0;
                if (!ParseU64C(s, v))
                    return false;
                *static_cast<u8*>(p) = static_cast<u8>(v);
                return true;
            }
            case ScalarKind::UInt16:
            {
                u64 v = 0;
                if (!ParseU64C(s, v))
                    return false;
                *static_cast<u16*>(p) = static_cast<u16>(v);
                return true;
            }
            case ScalarKind::UInt32:
            {
                u64 v = 0;
                if (!ParseU64C(s, v))
                    return false;
                *static_cast<u32*>(p) = static_cast<u32>(v);
                return true;
            }
            case ScalarKind::UInt64:
            {
                u64 v = 0;
                if (!ParseU64C(s, v))
                    return false;
                *static_cast<u64*>(p) = v;
                return true;
            }
            case ScalarKind::Float32:
            {
                char* end = nullptr;
                *static_cast<f32*>(p) = std::strtof(s, &end);
                return end != s;
            }
            case ScalarKind::Float64:
            {
                char* end = nullptr;
                *static_cast<f64*>(p) = std::strtod(s, &end);
                return end != s;
            }
            }
            return false;
        }

        static bool ParseI64(const char* s, i64& out)
        {
            char* end = nullptr;
            out = static_cast<i64>(std::strtoll(s, &end, 10));
            return end != s;
        }
        static bool ParseU64C(const char* s, u64& out)
        {
            char* end = nullptr;
            out = static_cast<u64>(std::strtoull(s, &end, 10));
            return end != s;
        }
        static bool ParseU64(StringView text, u64& out)
        {
            String t(text);
            return ParseU64C(reinterpret_cast<const char*>(t.CStr()), out);
        }

        static void EncodeHex(const u8* data, usize size, String& out)
        {
            static const char kHex[] = "0123456789abcdef";
            for (usize i = 0; i < size; ++i)
            {
                out.PushBack(static_cast<utf8char>(kHex[data[i] >> 4]));
                out.PushBack(static_cast<utf8char>(kHex[data[i] & 0xF]));
            }
        }
        static bool DecodeHex(const String& hex, u8* data, usize size)
        {
            if (hex.Size() != size * 2)
            {
                return false;
            }
            auto nibble = [](utf8char c) -> int
            {
                if (c >= u8'0' && c <= u8'9')
                    return c - u8'0';
                if (c >= u8'a' && c <= u8'f')
                    return c - u8'a' + 10;
                if (c >= u8'A' && c <= u8'F')
                    return c - u8'A' + 10;
                return -1;
            };
            for (usize i = 0; i < size; ++i)
            {
                const int hi = nibble(hex[i * 2]);
                const int lo = nibble(hex[i * 2 + 1]);
                if (hi < 0 || lo < 0)
                {
                    return false;
                }
                data[i] = static_cast<u8>((hi << 4) | lo);
            }
            return true;
        }

        XmlDocument m_doc; // owned (write mode; allocator from the ctor)
        XmlElement* m_writeCurrent = nullptr;
        Array<XmlElement*> m_writeStack;
        Array<ReadScope> m_readStack; // read mode
        const char* m_pendingKey = nullptr;
        // The key the last Locate looked for (null when it read the next element in order). It
        // is only read by a FailRead in the same call, while the caller's key is still alive.
        const char* m_lastKey = nullptr;
        String m_failure; // where the first read failure happened, in words
    };

    // =======================================================================
    // Built-in SerializerFactory for XML serialization.
    // =======================================================================
    namespace detail
    {
        struct XmlReadSerializerContext final : SerializerContext
        {
            XmlDocument doc{DefaultAllocator()}; // the factory signature carries no allocator
            XmlSerializer* xmlSer = nullptr;

            // SerializerFactory's fixed (IStream&, SerializeMode) signature carries no
            // allocator, so the contexts root on the process allocator.
            explicit XmlReadSerializerContext(IStream& stream) : doc(DefaultAllocator())
            {
                const u64 sz = stream.Size();
                Array<utf8char> buf(static_cast<usize>(sz));
                u64 n = stream.Read(buf.Data(), buf.Size());
                (void)n;
                auto pr = doc.Parse(StringView(buf.Data(), buf.Size()));
                (void)pr;
                xmlSer = DefaultAllocator().New<XmlSerializer>(doc);
                serializer = xmlSer;
            }

            ~XmlReadSerializerContext() override
            {
                if (xmlSer)
                {
                    DefaultAllocator().Delete(xmlSer);
                }
            }
        };

        struct XmlWriteSerializerContext final : SerializerContext
        {
            XmlSerializer* xmlSer = nullptr;

            explicit XmlWriteSerializerContext()
            {
                xmlSer = DefaultAllocator().New<XmlSerializer>(DefaultAllocator());
                serializer = xmlSer;
            }

            ~XmlWriteSerializerContext() override
            {
                if (xmlSer)
                {
                    DefaultAllocator().Delete(xmlSer);
                }
            }

            void Flush(IStream& out) override
            {
                if (!xmlSer)
                    return;
                String output;
                xmlSer->GetOutput(output);
                u64 n = out.Write(output.Data(), output.Size());
                (void)n;
            }
        };
    }

    [[nodiscard]] inline SerializerFactory XmlSerializerFactory()
    {
        return SerializerFactory{
            [](IStream& stream, SerializeMode mode) -> UniquePtr<SerializerContext>
            {
                SerializerContext* ctx = nullptr;
                if (mode == SerializeMode::Read)
                {
                    ctx = DefaultAllocator().New<detail::XmlReadSerializerContext>(stream);
                }
                else
                {
                    ctx = DefaultAllocator().New<detail::XmlWriteSerializerContext>();
                }
                return UniquePtr<SerializerContext>(ctx, DefaultAllocator());
            }};
    }
}
