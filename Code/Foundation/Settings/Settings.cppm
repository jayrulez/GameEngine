// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Settings - the `foundation.settings` module.
//
// A typed, versioned, backend-agnostic settings store. A "section" is
// a reflected ISerializable struct; the store holds one instance per type, lazily created (so an
// absent section reads as its struct defaults) and (de)serialized as a versioned payload keyed by the
// type's namespace and name. Load and Save go through the ISerializer/SerializerFactory abstraction:
// the caller picks the backend (XML for hand-editable files, binary for tests), and the store is not
// tied to any one. Streams only (no VFS): the caller opens the file (for example via VFS at
// core::GetUserDataDirectory()) and hands over the stream. Sits just above Core so the runtime and
// the editor can both use it.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h" // warn on a preserved or dropped unknown section

export module foundation.settings;

import foundation.core;
export import :save_values; // SaveValues: a game's keyed saved values, a section like any other

using namespace foundation::core;

export namespace foundation::settings
{
    // A store of typed settings sections. One instance holds one persisted file's worth (layering
    // across user and project files composes several Settings). Not copyable (owns live sections).
    class Settings
    {
    public:
        // The allocator (required; the owner decides) backs the lazily-created
        // sections and the section map.
        explicit Settings(IAllocator& allocator) noexcept
            : m_allocator(&allocator), m_sections(allocator)
        {
        }
        Settings(const Settings&) = delete;
        Settings& operator=(const Settings&) = delete;

        // The section of type T, lazily created (struct defaults) on first access. Stable reference
        // for the store's lifetime. T must be an ISerializable with a default ctor (RTTI_OBJECT).
        template <typename T>
        [[nodiscard]] T& Section()
        {
            const TypeId id = T::StaticType().id;
            if (RefPtr<ISerializable>* existing = m_sections.Find(id))
            {
                return static_cast<T&>(*existing->Get());
            }
            RefPtr<ISerializable> obj = MakeRef<T>(*m_allocator);
            T& ref = static_cast<T&>(*obj.Get());
            m_sections.InsertOrAssign(id, static_cast<RefPtr<ISerializable>&&>(obj));
            return ref;
        }

        // The section of type T if present (loaded or previously accessed); null otherwise. A
        // read-only peek that does not create the section.
        template <typename T>
        [[nodiscard]] const T* Find() const
        {
            const RefPtr<ISerializable>* existing = m_sections.Find(T::StaticType().id);
            return (existing != nullptr) ? static_cast<const T*>(existing->Get()) : nullptr;
        }

        // Announce that section T was mutated: fires OnChanged with T's type name. (The store can't
        // observe field writes through a T& reference, so callers signal explicitly.)
        template <typename T>
        void MarkChanged()
        {
            if (m_onChanged)
            {
                m_onChanged(StringView(reinterpret_cast<const utf8char*>(T::StaticType().name)));
            }
        }

        void OnChanged(Function<void(StringView)> cb)
        {
            m_onChanged = static_cast<Function<void(StringView)>&&>(cb);
        }

        [[nodiscard]] usize SectionCount() const noexcept { return m_sections.Size(); }

        // Sections whose type this build could not instantiate on the last Load, kept verbatim so the
        // next Save re-emits them (unknown-section passthrough). Zero on a store with all types known.
        [[nodiscard]] usize UnknownSectionCount() const noexcept { return m_unknownSections.Size(); }

        // Store format version, written into positional (binary) streams only; self-describing
        // backends (XML) rely on their structure. Bump when the binary section envelope changes.
        static constexpr u32 kFormatVersion = 2;

        // Serialize every live section to `out` through `factory` (envelope per section: type
        // namespace, name and versioned payload). Backend chosen by the caller.
        [[nodiscard]] Status Save(IStream& out, SerializerFactory factory) const
        {
            UniquePtr<SerializerContext> ctx = factory(out, SerializeMode::Write);
            if (!ctx || ctx->serializer == nullptr)
            {
                return Status{ErrorCode::Internal};
            }
            auto& ar =
                *ctx->serializer; // concrete Serializer (has IsOk/GetStatus, not on ISerializer)

            // Positional backends (binary) carry a store format version so a future layout change is
            // detectable; self-describing backends (XML) stay version-free.
            if (!ar.IsSelfDescribing())
            {
                u32 version = kFormatVersion;
                ar.Key("formatVersion");
                ar.Scalar(&version, ScalarKind::UInt32);
            }

            // Known live sections and any preserved unknown ones (both framed, so a reader that cannot
            // instantiate a section can skip or capture it).
            u32 count = static_cast<u32>(m_sections.Size() + m_unknownSections.Size());
            ar.Key("sections");
            ar.BeginArray(count);
            for (const auto& kv : m_sections)
            {
                ISerializable* obj = kv.value.Get();
                const TypeInfo& t = *obj->GetType();
                ar.BeginObject();
                String ns(reinterpret_cast<const utf8char*>(t.namespaceName));
                String name(reinterpret_cast<const utf8char*>(t.name));
                ar.Key("typeNamespace");
                ar.Text(ns);
                ar.Key("typeName");
                ar.Text(name);
                ar.BeginFramedRegion();
                BeginVersionedPayload(ar, t);
                ar.Key("payload");
                ar.BeginObject();
                obj->Serialize(ar);
                ar.EndObject();
                EndVersionedPayload(ar);
                ar.EndFramedRegion();
                ar.EndObject();
            }
            // Re-emit unknown sections verbatim inside a frame (preserves a newer file's sections when
            // an older build round-trips it). Appended after the known ones (order within kind stable).
            for (const UnknownSection& section : m_unknownSections)
            {
                ar.BeginObject();
                String ns = section.typeNamespace; // Text/RawRemainder take non-const lvalues
                String name = section.typeName;
                ar.Key("typeNamespace");
                ar.Text(ns);
                ar.Key("typeName");
                ar.Text(name);
                ar.BeginFramedRegion();
                Array<u8> payload = section.payload;
                ar.RawRemainder(payload);
                ar.EndFramedRegion();
                ar.EndObject();
            }
            ar.EndArray();
            if (!ar.IsOk())
            {
                return ar.GetStatus();
            }
            ctx->Flush(out);
            return Status{};
        }

        // Load sections from `in`. Each known and registered section type is instantiated and
        // deserialized; the store replaces any existing instance of that type. A section whose type
        // is unknown to this build is kept verbatim for the next Save when the backend can capture
        // its framed payload, and dropped with a warning when it cannot. Registered types must have
        // called RegisterSerializable<T>().
        [[nodiscard]] Status
        Load(IStream& in, SerializerFactory factory, TypeRegistry& types = GlobalTypeRegistry(),
             SerializableRegistry& serializables = GlobalSerializableRegistry())
        {
            UniquePtr<SerializerContext> ctx = factory(in, SerializeMode::Read);
            if (!ctx || ctx->serializer == nullptr)
            {
                return Status{ErrorCode::Internal};
            }
            auto& ar =
                *ctx->serializer; // concrete Serializer (has IsOk/GetStatus, not on ISerializer)

            m_unknownSections.Clear(); // rebuilt from this load's still-unknown sections

            // Positional backends (binary) begin with the store format version; refuse a stream that
            // isn't the current version rather than misparsing it. Self-describing backends (XML) have
            // no such field.
            if (!ar.IsSelfDescribing())
            {
                u32 version = 0;
                ar.Key("formatVersion");
                ar.Scalar(&version, ScalarKind::UInt32);
                if (!ar.IsOk() || version != kFormatVersion)
                {
                    return Status{ErrorCode::NotSupported};
                }
            }

            u32 count = 0;
            ar.Key("sections");
            ar.BeginArray(count);
            for (u32 i = 0; i < count; ++i)
            {
                ar.BeginObject();
                String ns;
                String name;
                ar.Key("typeNamespace");
                ar.Text(ns);
                ar.Key("typeName");
                ar.Text(name);
                if (!ar.IsOk())
                {
                    return ar.GetStatus();
                }

                const TypeInfo* type = types.FindByName(reinterpret_cast<const char*>(ns.CStr()),
                                                        reinterpret_cast<const char*>(name.CStr()));
                RefPtr<ISerializable> obj =
                    (type != nullptr) ? serializables.Create(type->id) : RefPtr<ISerializable>{};

                ar.BeginFramedRegion();
                if (obj.Get() != nullptr)
                {
                    BeginVersionedPayload(ar, *type);
                    ar.Key("payload");
                    ar.BeginObject();
                    obj->Serialize(ar);
                    ar.EndObject();
                    EndVersionedPayload(ar);
                    m_sections.InsertOrAssign(type->id, static_cast<RefPtr<ISerializable>&&>(obj));
                }
                else
                {
                    // An unknown or unregistered type: capture the framed payload verbatim so a later
                    // Save preserves it, rather than aborting the whole store. A backend that cannot
                    // preserve it (positional with no frame) drops it with a warning.
                    Array<u8> payload;
                    if (ar.RawRemainder(payload))
                    {
                        UnknownSection section;
                        section.typeNamespace = ns;
                        section.typeName = name;
                        section.payload = static_cast<Array<u8>&&>(payload);
                        m_unknownSections.PushBack(static_cast<UnknownSection&&>(section));
                        LOG_WARNING(u8"Settings",
                                             u8"preserving unknown settings section '{}::{}'",
                                             ns.AsView(), name.AsView());
                    }
                    else
                    {
                        LOG_WARNING(
                            u8"Settings",
                            u8"dropping unknown settings section '{}::{}' (backend cannot preserve it)",
                            ns.AsView(), name.AsView());
                    }
                }
                ar.EndFramedRegion();
                ar.EndObject();
                if (!ar.IsOk())
                {
                    return ar.GetStatus();
                }
            }
            ar.EndArray();
            return ar.IsOk() ? Status{} : ar.GetStatus();
        }

        // A section kept verbatim because its type was unknown at Load. Re-emitted by Save inside a
        // frame so a newer settings file survives an older build's load and save round-trip intact.
        struct UnknownSection
        {
            String typeNamespace;
            String typeName;
            Array<u8> payload; // XML: the captured element subtree bytes; binary: the framed bytes
        };

    private:
        IAllocator* m_allocator;
        HashMap<TypeId, RefPtr<ISerializable>> m_sections;
        Array<UnknownSection> m_unknownSections;
        Function<void(StringView)> m_onChanged;
    };
}
