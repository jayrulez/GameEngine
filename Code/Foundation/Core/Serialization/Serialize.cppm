// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :serialize partition
//
// Serialize(ISerializer&, T&) free functions for Core's own types: describe a
// type's data once, runs either direction and works for any backend (binary or
// keyed/text). Aggregates use named fields / array scopes so text output stays
// readable and binary output stays endianness-portable.

module;
#include "Core/Prelude.h"
#include <type_traits>

export module foundation.core:serialize;

import :base;
import :serializer;
import :iserializable;
import :type_info; // versioned payloads (data-version chains)
import :math;
import :color;
import :float2;
import :float3;
import :float4;
import :float4x4;
import :quaternion;
import :string;
import :array;
import :guid;

export namespace foundation::core
{
    // -----------------------------------------------------------------------
    // Scalar type mapping - C++ type to ScalarKind (enums via underlying type).
    // -----------------------------------------------------------------------
    template <typename T>
    constexpr ScalarKind ScalarKindOf() noexcept
    {
        if constexpr (std::is_enum_v<T>)
        {
            // Resolve via the underlying type (only instantiated for enums).
            return ScalarKindOf<std::underlying_type_t<T>>();
        }
        else if constexpr (std::is_same_v<T, bool>)
        {
            return ScalarKind::Bool;
        }
        else if constexpr (std::is_floating_point_v<T>)
        {
            return sizeof(T) == 4 ? ScalarKind::Float32 : ScalarKind::Float64;
        }
        else if constexpr (std::is_signed_v<T>)
        {
            if constexpr (sizeof(T) == 1)
            {
                return ScalarKind::Int8;
            }
            else if constexpr (sizeof(T) == 2)
            {
                return ScalarKind::Int16;
            }
            else if constexpr (sizeof(T) == 4)
            {
                return ScalarKind::Int32;
            }
            else
            {
                return ScalarKind::Int64;
            }
        }
        else
        {
            if constexpr (sizeof(T) == 1)
            {
                return ScalarKind::UInt8;
            }
            else if constexpr (sizeof(T) == 2)
            {
                return ScalarKind::UInt16;
            }
            else if constexpr (sizeof(T) == 4)
            {
                return ScalarKind::UInt32;
            }
            else
            {
                return ScalarKind::UInt64;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Serialize() - describe a type's data once; runs either direction.
    // -----------------------------------------------------------------------

    // Arithmetic and enum types: one typed scalar.
    template <typename T>
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    void Serialize(ISerializer& ar, T& value)
    {
        ar.Scalar(&value, ScalarKindOf<T>());
    }

    // Math value types: named fields, so text output is readable and binary is
    // decomposed (no struct-padding / endianness surprises).
    inline void Serialize(ISerializer& ar, Float2& v)
    {
        ar.BeginObject();
        ar.Key("x");
        Serialize(ar, v.x);
        ar.Key("y");
        Serialize(ar, v.y);
        ar.EndObject();
    }
    inline void Serialize(ISerializer& ar, Float3& v)
    {
        ar.BeginObject();
        ar.Key("x");
        Serialize(ar, v.x);
        ar.Key("y");
        Serialize(ar, v.y);
        ar.Key("z");
        Serialize(ar, v.z);
        ar.EndObject();
    }
    inline void Serialize(ISerializer& ar, Float4& v)
    {
        ar.BeginObject();
        ar.Key("x");
        Serialize(ar, v.x);
        ar.Key("y");
        Serialize(ar, v.y);
        ar.Key("z");
        Serialize(ar, v.z);
        ar.Key("w");
        Serialize(ar, v.w);
        ar.EndObject();
    }
    inline void Serialize(ISerializer& ar, Color& c)
    {
        ar.BeginObject();
        ar.Key("r");
        Serialize(ar, c.r);
        ar.Key("g");
        Serialize(ar, c.g);
        ar.Key("b");
        Serialize(ar, c.b);
        ar.Key("a");
        Serialize(ar, c.a);
        ar.EndObject();
    }
    inline void Serialize(ISerializer& ar, Quaternion& q)
    {
        ar.BeginObject();
        ar.Key("x");
        Serialize(ar, q.x);
        ar.Key("y");
        Serialize(ar, q.y);
        ar.Key("z");
        Serialize(ar, q.z);
        ar.Key("w");
        Serialize(ar, q.w);
        ar.EndObject();
    }
    inline void Serialize(ISerializer& ar, Float4x4& m)
    {
        // 16 elements, row-major.
        u32 count = 16;
        ar.BeginArray(count);
        const u32 n = count < 16u ? count : 16u;
        for (u32 i = 0; i < n; ++i)
        {
            Serialize(ar, m.Data()[i]);
        }
        ar.EndArray();
    }

    // Guid: a backend-chosen primitive: compact raw 16 bytes in binary, canonical
    // 36-char string in text. One copyable value, usable anywhere a guid reference is needed.
    inline void Serialize(ISerializer& ar, Guid& g) { ar.GuidValue(g); }

    // ISerializable: dispatch to the object's own Serialize(), so serializable
    // members compose with Serialize(ar, "key", member) like any other type.
    inline void Serialize(ISerializer& ar, ISerializable& obj) { obj.Serialize(ar); }

    // String: first-class text (UTF-8).
    inline void Serialize(ISerializer& ar, String& str) { ar.Text(str); }

    // Array: count-prefixed (array scope), each element serialized via Serialize().
    template <typename T>
    void Serialize(ISerializer& ar, Array<T>& array)
    {
        u32 count = static_cast<u32>(array.Size());
        ar.BeginArray(count);
        if (ar.Mode() == SerializeMode::Read && !ar.IsPayloadOk())
        {
            count = 0; // the read has failed: the count it read is not data
        }

        // Scalars (and byte) in a backend whose element layout is the raw run: one Blob for the
        // whole array. Byte-identical to the per-element loop below (each element would be one
        // Scalar of exactly sizeof(T) bytes), so the on-disk format is the same, with one virtual
        // call per array instead of one per element (a large vertex blob is slow at -O0 otherwise).
        constexpr bool kScalarRun = (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) ||
                                    std::is_same_v<T, byte>;
        if constexpr (kScalarRun)
        {
            if (ar.BulkScalarArrays())
            {
                if (ar.Mode() == SerializeMode::Read)
                {
                    array.Resize(count);
                }
                if (count > 0)
                {
                    ar.Blob(array.Data(), static_cast<usize>(count) * sizeof(T));
                }
                ar.EndArray();
                return;
            }
        }
        if (ar.Mode() == SerializeMode::Read)
        {
            array.Clear();
            array.Reserve(count);
            for (u32 i = 0; i < count; ++i)
            {
                T element{};
                Serialize(ar, element);
                if (!ar.IsPayloadOk())
                {
                    break; // stop at the first failure; the rest would read past it
                }
                array.PushBack(Move(element));
            }
        }
        else
        {
            for (u32 i = 0; i < count; ++i)
            {
                Serialize(ar, array[i]);
            }
        }

        ar.EndArray();
    }

    // Named-field convenience for object members: `Serialize(ar, "key", value)`.
    template <typename T>
    void Serialize(ISerializer& ar, const char* key, T& value)
    {
        ar.Key(key);
        Serialize(ar, value);
    }

    // A field appended to a layout already in use, which a keyed (text) payload written before
    // it existed does not have: reading one, the field keeps its default rather than failing
    // the whole read, so the files a project has already saved still load.
    //
    // Text only. A positional (binary) payload cannot tell a missing field from the next one,
    // so there it is read like any other; binary data that predates a field still needs a
    // version bump. For a type stored as text, a manifest or a settings file, this is how its
    // shape grows without refusing every file saved under the old one.
    template <typename T>
    void SerializeAppended(ISerializer& ar, const char* key, T& value)
    {
        if (ar.Mode() != SerializeMode::Read || ar.HasKey(key))
        {
            Serialize(ar, key, value);
        }
    }
}

// === Versioned payloads (one supported layout per type: the current one) ================
//
// Wrap a payload whose layout is stamped with the type's data version:
//     BeginVersionedPayload(ar, TypeOf<T>());   // or object->GetType()
//     ... Serialize fields (the current layout, unconditionally) ...
//     EndVersionedPayload(ar);
// Write: emits the type's data-version chain (concrete first, then every versioned base).
// Read: parses the stored chain and requires it to equal the current chain; a payload written
// under any other version fails (ErrorCode::NotSupported). One allowance: a type that keeps a
// legacy reader (TypeInfo::minReadDataVersion, TypeBuilder::ReadsDataVersionsFrom) accepts an
// older concrete version down to that floor, and the pushed scope then reports the stored
// version so its Serialize body can branch (ar.Version()). Bases stay exact.
export namespace foundation::core
{
    inline void BeginVersionedPayload(ISerializer& ar, const TypeInfo& type)
    {
        // Chain: concrete type always; bases only when versioned (compact and stable).
        SerializedDataVersion current[16];
        u32 currentCount = 0;
        current[currentCount++] = SerializedDataVersion{type.id, type.dataVersion};
        for (const TypeInfo* base = type.base; base != nullptr && currentCount < 16;
             base = base->base)
        {
            if (base->dataVersion > 0)
            {
                current[currentCount++] = SerializedDataVersion{base->id, base->dataVersion};
            }
        }

        SerializedDataVersion chain[16];
        u32 count = currentCount;
        if (ar.Mode() == SerializeMode::Write)
        {
            for (u32 i = 0; i < count; ++i)
            {
                chain[i] = current[i];
            }
        }
        ar.Key("dataVersions");
        ar.BeginArray(count); // read: filled with the stored count
        const u32 read = count < 16 ? count : 16;
        for (u32 i = 0; i < read; ++i)
        {
            ar.Key("type");
            ar.Scalar(&chain[i].typeId, ScalarKind::UInt64);
            ar.Key("version");
            ar.Scalar(&chain[i].version, ScalarKind::UInt32);
        }
        ar.EndArray();
        if (ar.Mode() == SerializeMode::Read)
        {
            bool matches = count == currentCount;
            for (u32 i = 0; matches && i < read; ++i)
            {
                const bool legacy = i == 0 && type.readsLegacyData &&
                                    chain[i].version >= type.minReadDataVersion &&
                                    chain[i].version < current[i].version;
                matches = chain[i].typeId == current[i].typeId &&
                          (chain[i].version == current[i].version || legacy);
            }
            if (!matches)
            {
                ar.FailPayload(ErrorCode::NotSupported); // stale (or foreign) data version
            }
        }
        ar.PushVersionScope(chain, read);
    }

    inline void EndVersionedPayload(ISerializer& ar) { ar.PopVersionScope(); }
}
