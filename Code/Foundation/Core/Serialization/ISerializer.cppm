// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :iserializer partition
//
// The serialization contract: a mode-aware, format-agnostic interface. One
// Serialize() path runs either direction, and the interface speaks in *intent*
// (typed scalars, named fields, structured scopes) rather than raw bytes, so a
// binary backend and a keyed/text backend (JSON, …) can both implement it.

module;
#include "Core/Prelude.h"

export module foundation.core:iserializer;

import :base;
import :array; // RawRemainder's raw payload
import :string;
import :guid;

export namespace foundation::core
{
    enum class SerializeMode
    {
        Read,
        Write,
    };

    // The type of a scalar, so keyed/text backends can emit the right token and
    // binary backends know the width. Decouples "what kind of value" from bytes.
    enum class ScalarKind : u8
    {
        Bool,
        Int8,
        UInt8,
        Int16,
        UInt16,
        Int32,
        UInt32,
        Int64,
        UInt64,
        Float32,
        Float64,
    };

    // Format-agnostic pure interface. Backends extend Serializer, not this
    // directly: Serializer supplies no-op defaults for the naming/scope ops
    // that unkeyed formats (e.g. binary) ignore.
    //
    // Keyed/text backends use Key()/object scopes to produce `"name": value`.
    // One entry of a data-version scope: the stable type id and the version the data carries
    // (write: the type's current version; read: whatever the stream stored).
    struct SerializedDataVersion
    {
        u64 typeId = 0;
        u32 version = 0;
    };

    class ISerializer
    {
    public:
        virtual ~ISerializer() = default;

        [[nodiscard]] virtual SerializeMode Mode() const noexcept = 0;

        // === Data-version scopes ===
        // A scope is pushed around each versioned payload (content envelopes, scene component
        // records) with the version chain the data carries. BeginVersionedPayload refuses a
        // stored chain that differs from the type's current one, so inside a payload Version()
        // reports the current data version and Serialize bodies read one layout. The exception is
        // a type that keeps a legacy reader (TypeInfo::minReadDataVersion): its scope carries
        // the stored version so the body can branch on it for the old layout. Version() is the
        // payload's concrete type's; Version(typeId) is a base class's in the chain (0 when
        // absent, since unversioned data reads as version 0).
        [[nodiscard]] virtual u32 Version() const noexcept = 0;
        [[nodiscard]] virtual u32 Version(u64 typeId) const noexcept = 0;
        virtual void PushVersionScope(const SerializedDataVersion* chain, usize count) = 0;
        virtual void PopVersionScope() = 0;

        // Names the next value within the current object. Ignored by unkeyed
        // formats; keyed/text formats associate it with the value that follows.
        virtual void Key(const char* name) noexcept = 0;
        // Reading: whether the current object has a value under `name` still to be read, so an
        // appended field (SerializeAppended) can tell a payload from before it. A positional
        // format cannot tell, and says yes, reading on as it always does.
        [[nodiscard]] virtual bool HasKey(const char* name) const
        {
            (void)name;
            return true;
        }

        // Structured scopes. BeginArray moves the element count (written on
        // write, read on read).
        virtual void BeginObject() = 0;
        virtual void EndObject() = 0;
        virtual void BeginArray(u32& count) = 0;
        virtual void EndArray() = 0;

        // Moves one typed scalar between memory and the backing store.
        virtual void Scalar(void* value, ScalarKind kind) = 0;

        // Moves a string. First-class so text formats store it natively.
        virtual void Text(String& value) = 0;

        // Moves an opaque byte blob (raw in binary; e.g. base64 in text).
        virtual void Blob(void* data, usize size) = 0;

        // Moves a Guid as a backend-chosen primitive: binary writes the raw 16
        // bytes (compact); text writes the canonical 36-char string (readable, one copyable value).
        // The Serializer base provides the text default; binary overrides for compactness.
        virtual void GuidValue(Guid& value) = 0;

        // === Payload-level validation (strict versioning) ===
        // A Serialize body that detects structurally-invalid data after reading (a cross-field
        // invariant a positional format cannot express) fails the whole payload here, so corrupt
        // data loads loudly, never silently. The Serializer base routes this into its status;
        // the defaults keep bare mocks working.
        virtual void FailPayload(ErrorCode code) noexcept { (void)code; }

        // Unknown-section passthrough (see Serializer for the contract): capture (read) or
        // re-inject (write) the current region's remaining content verbatim, so a payload whose
        // type this build cannot instantiate survives a round-trip. Declared on the interface
        // so consumers holding an ISerializer& (scene records) can preserve unknown types;
        // unsupported by default; Serializer's backends override.
        virtual bool RawRemainder(Array<u8>& blob)
        {
            (void)blob;
            return false;
        }
        [[nodiscard]] virtual bool IsPayloadOk() const noexcept { return true; }
        // True when a run of scalar elements is laid out exactly as its raw bytes (the binary
        // backend): Serialize(Array<scalar>) then moves the whole run through Blob in one call
        // instead of one virtual Scalar per element. The bytes are the same, so the format does
        // not change.
        [[nodiscard]] virtual bool BulkScalarArrays() const noexcept { return false; }
    };
}
