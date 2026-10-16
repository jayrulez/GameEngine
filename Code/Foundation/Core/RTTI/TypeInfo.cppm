// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :type_info partition
//
// The type-system foundation: stable type identity (TypeId / TypeInfo),
// ComputeTypeId, and TypeOf<T>. Registry, Object, casting, and the reflection
// runtime build on this.

module;
#include "Core/Prelude.h"

export module foundation.core:type_info;

import :base;
import :hash;
import :string;
import :guid;

export namespace foundation::core
{
    using TypeId = u64;

    struct PropertyInfo;    // fully defined in :reflection
    struct MethodInfo;      // fully defined in :reflection
    struct Attribute;       // fully defined in :reflection
    struct ContainerInfo;   // fully defined in :reflection
    struct ConstantInfo;    // fully defined in :reflection (named static values)
    struct ConstructorInfo; // fully defined in :reflection
    struct CreatorInfo;     // fully defined in :reflection

    struct EnumValue
    {
        const char* name;
        i64 value;
    };

    struct TypeInfo;

    /// What generic tooling does with a reference-shaped value, type-erased: the Guid inside,
    /// setting it, dropping any runtime binding so the next resolve binds the new id, and
    /// the type the reference points at.
    struct ReferenceOps
    {
        const Guid* (*Id)(const void* value);
        void (*SetId)(void* value, const Guid& id);
        void (*ClearBinding)(void* value);
        /// The type the reference points at (a resource's runtime type for Ref<T>): what a
        /// schema or a picker needs to say WHAT kind of thing the guid names, without naming
        /// the reference template. Capability flowing into reflection at registration.
        const TypeInfo* (*Target)();
    };

    struct TypeInfo
    {
        TypeId id;
        const char* name;          // unqualified, e.g. "Entity"
        const char* namespaceName; // e.g. "rtti::game"
        u32 size;
        u32 align;
        const TypeInfo* base;                     // single-inheritance chain; null at the root
        const PropertyInfo* properties = nullptr; // declared in this type (not inherited)
        u32 propertyCount = 0;
        const MethodInfo* methods = nullptr;
        u32 methodCount = 0;
        const EnumValue* enumerators = nullptr; // populated for reflected enums
        u32 enumeratorCount = 0;
        const Attribute* attributes = nullptr;
        u32 attributeCount = 0;
        const ContainerInfo* container = nullptr; // non-null for reflected containers
        const ConstantInfo* constants = nullptr;  // named static values (e.g. Float3::Zero)
        u32 constantCount = 0;
        const ConstructorInfo* constructors = nullptr; // reflected constructors (overloads)
        u32 constructorCount = 0;
        // How native code makes one from an owner's allocator (TypeBuilder::Creator), for an
        // Object type created by name (a GUI element from markup); null when the type declares
        // none. Not a constructor: script binders never bind it.
        const CreatorInfo* creator = nullptr;
        // DATA version of the serialized layout: bump when it changes. The reader accepts the
        // current version (BeginVersionedPayload), and older ones only down to
        // `minReadDataVersion` when a type keeps a legacy reader (its Serialize body branches
        // on ar.Version() for those). 0 = never versioned. Set via RTTI_DEFINE_OBJECT_VERSIONED
        // or TypeBuilder::DataVersion.
        u32 dataVersion = 0;
        // The oldest stored data version the reader still accepts (0 = the current one only).
        // TypeBuilder::ReadsDataVersionsFrom; a legacy reader is one version back, named in its
        // commit and removed once the data has moved (Process/CONVENTIONS.md).
        u32 minReadDataVersion = 0;
        // A reference-shaped value (a resource::Ref<T>, whatever T): its identity is a Guid,
        // and generic tooling reads and writes THAT instead of the value. The ops come from
        // ReferenceTraits<T> when the type's TypeInfo is made; null for every other type.
        const ReferenceOps* reference = nullptr;
    };

    /// The customization point a module specializes for its reference template (Resource does
    /// for Ref<T>): `isReference` true and `Ops()` the table. Every TypeInfo made for such a
    /// type carries it, so a Variant, a property or a container element of it is read and
    /// written by identity without anyone naming the template.
    template <typename T>
    struct ReferenceTraits
    {
        static constexpr bool isReference = false;
    };

    [[nodiscard]] inline bool IsReferenceType(const TypeInfo& type) noexcept
    {
        return type.reference != nullptr;
    }

    // Stable 64-bit identity from the fully-qualified name.
    [[nodiscard]] inline TypeId ComputeTypeId(const char* namespaceName, const char* name) noexcept
    {
        u64 hash = HashBytes(namespaceName, CStringLength(namespaceName));
        hash = HashBytes("::", 2, hash);
        hash = HashBytes(name, CStringLength(name), hash);
        return hash;
    }

    template <typename T>
    [[nodiscard]] TypeInfo MakeTypeInfo(const char* name, const char* namespaceName,
                                        const TypeInfo* base, u32 dataVersion = 0) noexcept
    {
        TypeInfo info{
            ComputeTypeId(namespaceName, name), name, namespaceName, static_cast<u32>(sizeof(T)),
            static_cast<u32>(alignof(T)),       base};
        info.dataVersion = dataVersion;
        if constexpr (ReferenceTraits<T>::isReference)
        {
            info.reference = &ReferenceTraits<T>::Ops();
        }
        return info;
    }

    namespace detail
    {
        // FNV-1a over the compiler's signature string for this instantiation: distinct
        // per T, and - unlike an address - identical in every translation unit AND every
        // shared library produced by one toolchain. This is the RUNTIME identity for
        // unregistered value types; it is never serialized (disk formats go through
        // authored names). See Documentation/Specs/shared-libraries.md.
        template <typename T>
        [[nodiscard]] consteval TypeId SignatureTypeId() noexcept
        {
#if COMPILER_MSVC
            const char* s = __FUNCSIG__;
#else
            const char* s = __PRETTY_FUNCTION__;
#endif
            u64 hash = kFnv1a64OffsetBasis;
            for (; *s != '\0'; ++s)
            {
                hash = (hash ^ static_cast<u64>(static_cast<unsigned char>(*s))) * kFnv1a64Prime;
            }
            return hash == 0 ? 1 : hash;
        }
    }

    namespace detail
    {
        // The process-single storage behind TypeOf<T>(): ONE TypeInfo per signature id,
        // owned by Core's implementation unit (RTTI/TypeInfoImpl.cpp) in static storage
        // (usable from the first call in static init; nothing to tear down). A
        // template's function-local static is one instance PER IMAGE (PE always; ELF
        // once the template is hidden), so if the TypeInfo itself lived there the
        // metadata REFLECT_VALUE / EnumBuilder / container registrars patch inside one
        // library would be invisible to every other library - the W1 finding in
        // shared-libraries.md. Routing every image's first use through here makes
        // &TypeOf<T>() one address per process again. `prototype` supplies the layout
        // facts on first sight and refreshes them for a rebuilt module (hot reload);
        // patched metadata is never touched here (last registrar wins).
        [[nodiscard]] TypeInfo& TypeInfoSlot(TypeId signatureId,
                                             const TypeInfo& prototype) noexcept;

        // What TypeOf<T>() hands the slot on first use in an image: the layout facts
        // plus the signature id (the RUNTIME identity for unregistered value types).
        template <typename T>
        [[nodiscard]] TypeInfo ValuePrototype() noexcept
        {
            TypeInfo t = MakeTypeInfo<T>("<value>", "", nullptr);
            t.id = SignatureTypeId<T>();
            return t;
        }
    }

    // Lazily-created TypeInfo for any value type not covered by REFLECT_VALUE /
    // REFLECT_ENUM; used by Variant/Instance for type checks. Object-derived types
    // should prefer their StaticType() instead. The id is the compile-time signature
    // hash above, so identity survives shared-library boundaries. Registration
    // (REFLECT_VALUE/EnumBuilder) still overwrites id + name with the authored ones -
    // and because the TypeInfo is process-single (detail::TypeInfoSlot), that patch is
    // what every library sees. Only the cached REFERENCE below is per image, which is
    // why the template is COMPILER_ATTR_HIDDEN: duplicating it is the design, and
    // hiding it makes the Linux shared lane prove the rendezvous the way PE does.
    template <typename T>
    [[nodiscard]] COMPILER_ATTR_HIDDEN const TypeInfo& TypeOf() noexcept
    {
        static TypeInfo& info =
            detail::TypeInfoSlot(detail::SignatureTypeId<T>(), detail::ValuePrototype<T>());
        return info;
    }
}
