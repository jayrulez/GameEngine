// Draconic Foundation - :type_info partition
//
// The type-system foundation: stable type identity (TypeId / TypeInfo),
// ComputeTypeId, and TypeOf<T>. Registry, Object, casting, and the reflection
// runtime build on this (Documentation/Planning/Core.md §4.10).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:type_info;

import :base;
import :hash;
import :string;

export namespace draconic::foundation
{
    using TypeId = u64;

    struct PropertyInfo;    // fully defined in :reflection
    struct MethodInfo;      // fully defined in :reflection
    struct Attribute;       // fully defined in :reflection
    struct ContainerInfo;   // fully defined in :reflection
    struct ConstantInfo;    // fully defined in :reflection (named static values)
    struct ConstructorInfo; // fully defined in :reflection

    struct EnumValue
    {
        const char* name;
        i64 value;
    };

    struct TypeInfo
    {
        TypeId id;
        const char* name;          // unqualified, e.g. "Entity"
        const char* namespaceName; // e.g. "draconic::game"
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
        // DATA version for serialization migration (Traktor-style): bump when the type's
        // serialized layout changes; Serialize bodies branch on ar.Version() for old data.
        // 0 = never versioned. Set via DRACONIC_DEFINE_OBJECT_VERSIONED or
        // TypeBuilder::DataVersion.
        u32 dataVersion = 0;
    };

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
        return info;
    }

    // Lazily-created TypeInfo for any value type. Identity is the returned
    // object's address (process-stable); used by Variant/Instance for type
    // checks. Object-derived types should prefer their StaticType() instead.
    // (A nice name / stable hashed id for value types comes in a later phase.)
    template <typename T>
    [[nodiscard]] const TypeInfo& TypeOf() noexcept
    {
        static TypeInfo info = MakeTypeInfo<T>("<value>", "", nullptr);
        static const bool initialized = []() noexcept
        {
            info.id = static_cast<TypeId>(reinterpret_cast<uptr>(&info));
            return true;
        }();
        (void)initialized;
        return info;
    }
}
