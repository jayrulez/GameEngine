// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Core - :schema_recorder partition.
//
// SchemaRecorder: an ISerializer backend in write mode that stores nothing and records what a
// Serialize body did: each Key with the scalar kind and the value that followed it, the
// nesting of objects and arrays, text, guid and blob fields, and the data-version chain a
// versioned payload pushed. Driven through the real write path over a default-constructed
// instance, the recording is the wire format: the order, keys, kinds and defaults the reader
// will expect, taken from the code that writes real files rather than from reflection (which
// differs from the wire in field set, order, encoding and defaults).
module;
#include "Core/Prelude.h"

export module foundation.core:schema_recorder;

import :base;
import :string;
import :guid;
import :array;
import :span;
import :unique_ptr;
import :allocator;
import :format;
import :serializer;

export namespace foundation::core
{
    enum class SchemaNodeKind : u8
    {
        Object, ///< BeginObject .. EndObject (the root is one)
        Array,  ///< BeginArray .. EndArray; the children are the elements written, in order
        Scalar, ///< one typed scalar; `scalar` says which, `value` its text
        Text,   ///< a string; `value` is it
        Guid,   ///< a guid; `value` is its canonical 36-character text
        Blob,   ///< opaque bytes; `blobSize` is how many
    };

    /// One thing a Serialize body wrote, in the order it wrote it.
    struct SchemaNode
    {
        String key;        ///< the Key() that preceded it; empty for an array element or the root
        SchemaNodeKind kind = SchemaNodeKind::Object;
        ScalarKind scalar = ScalarKind::Bool; ///< for Scalar
        String value;      ///< the value written, as text (a default-constructed instance's default)
        u32 count = 0;     ///< for Array: the element count written
        usize blobSize = 0; ///< for Blob
        Array<UniquePtr<SchemaNode>> children; ///< for Object and Array

        /// The first child with `name` (an object's field by key); null when none.
        [[nodiscard]] const SchemaNode* Find(StringView name) const noexcept
        {
            for (const UniquePtr<SchemaNode>& child : children)
            {
                if (child->key.AsView() == name)
                {
                    return child.Get();
                }
            }
            return nullptr;
        }
        [[nodiscard]] const SchemaNode* At(usize index) const noexcept
        {
            return index < children.Size() ? children[index].Get() : nullptr;
        }
    };

    class SchemaRecorder final : public Serializer
    {
    public:
        explicit SchemaRecorder(IAllocator& allocator) noexcept
            : Serializer(SerializeMode::Write), m_allocator(&allocator)
        {
            m_root = MakeUnique<SchemaNode>(allocator);
            m_root->kind = SchemaNodeKind::Object;
            m_stack.PushBack(m_root.Get());
        }

        /// The recording: an implicit root object holding what the body wrote at its top level.
        [[nodiscard]] const SchemaNode& Root() const noexcept { return *m_root; }
        /// The data-version chain of the outermost versioned payload recorded (concrete type
        /// first, then every versioned base), as BeginVersionedPayload pushed it; empty when
        /// the body was not versioned.
        [[nodiscard]] Span<const SerializedDataVersion> VersionChain() const noexcept
        {
            return Span<const SerializedDataVersion>{m_chain.Data(), m_chain.Size()};
        }

        // === ISerializer: every operation records and moves nothing ===

        void Key(const char* name) noexcept override
        {
            m_pendingKey = String(StringView(reinterpret_cast<const utf8char*>(name)));
        }
        void BeginObject() override
        {
            SchemaNode& node = Add(SchemaNodeKind::Object);
            m_stack.PushBack(&node);
        }
        void EndObject() override { Pop(); }
        void BeginArray(u32& count) override
        {
            SchemaNode& node = Add(SchemaNodeKind::Array);
            node.count = count;
            m_stack.PushBack(&node);
        }
        void EndArray() override { Pop(); }
        void Scalar(void* value, ScalarKind kind) override
        {
            SchemaNode& node = Add(SchemaNodeKind::Scalar);
            node.scalar = kind;
            node.value = ScalarText(value, kind);
        }
        void Text(String& value) override
        {
            SchemaNode& node = Add(SchemaNodeKind::Text);
            node.value = value;
        }
        void GuidValue(Guid& value) override
        {
            SchemaNode& node = Add(SchemaNodeKind::Guid);
            utf8char text[37];
            value.ToChars(text);
            node.value = String(StringView(text, 36));
        }
        void Blob(void* data, usize size) override
        {
            (void)data;
            SchemaNode& node = Add(SchemaNodeKind::Blob);
            node.blobSize = size;
        }
        void PushVersionScope(const SerializedDataVersion* chain, usize count) override
        {
            if (m_chain.IsEmpty() && !m_chainTaken)
            {
                for (usize i = 0; i < count; ++i)
                {
                    m_chain.PushBack(chain[i]);
                }
                m_chainTaken = true;
            }
            Serializer::PushVersionScope(chain, count);
        }
        [[nodiscard]] bool IsSelfDescribing() const noexcept override { return true; }

        /// A scalar's text the way the schema shows a default: booleans as true/false, integers
        /// as decimal, floats as the shortest round-trip decimal Format gives.
        [[nodiscard]] static String ScalarText(const void* value, ScalarKind kind)
        {
            switch (kind)
            {
            case ScalarKind::Bool:
                return String(*static_cast<const bool*>(value) ? u8"true" : u8"false");
            case ScalarKind::Int8:
                return Format(u8"{}", static_cast<i32>(*static_cast<const i8*>(value)));
            case ScalarKind::UInt8:
                return Format(u8"{}", static_cast<u32>(*static_cast<const u8*>(value)));
            case ScalarKind::Int16:
                return Format(u8"{}", static_cast<i32>(*static_cast<const i16*>(value)));
            case ScalarKind::UInt16:
                return Format(u8"{}", static_cast<u32>(*static_cast<const u16*>(value)));
            case ScalarKind::Int32:
                return Format(u8"{}", *static_cast<const i32*>(value));
            case ScalarKind::UInt32:
                return Format(u8"{}", *static_cast<const u32*>(value));
            case ScalarKind::Int64:
                return Format(u8"{}", *static_cast<const i64*>(value));
            case ScalarKind::UInt64:
                return Format(u8"{}", *static_cast<const u64*>(value));
            case ScalarKind::Float32:
                return Format(u8"{}", *static_cast<const f32*>(value)); // its own shortest form
            case ScalarKind::Float64:
                return Format(u8"{}", *static_cast<const f64*>(value));
            }
            return String();
        }

    private:
        SchemaNode& Add(SchemaNodeKind kind)
        {
            UniquePtr<SchemaNode> node = MakeUnique<SchemaNode>(*m_allocator);
            node->kind = kind;
            node->key = Move(m_pendingKey);
            m_pendingKey.Clear();
            SchemaNode* raw = node.Get();
            m_stack[m_stack.Size() - 1]->children.PushBack(Move(node));
            return *raw;
        }
        void Pop()
        {
            if (m_stack.Size() > 1)
            {
                m_stack.PopBack(); // the root never pops: an unbalanced End is the body's bug, not a crash
            }
        }

        IAllocator* m_allocator;
        UniquePtr<SchemaNode> m_root;
        Array<SchemaNode*> m_stack; // the open object or array; nodes are pointer-stable (UniquePtr-owned)
        String m_pendingKey;
        Array<SerializedDataVersion> m_chain;
        bool m_chainTaken = false;
    };
}
