// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :reflection partition
//
// Reflection runtime built on Variant/Instance: properties, methods, enums'
// attributes, container reflection, and the TypeBuilder used by REFLECT_MEMBERS.

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"
#include <type_traits>
#include <utility>

export module foundation.core:reflection;

import :base;
import :allocator;
import :array;
import :span;
import :type_info;
import :variant;
import :instance;
import :object;
import :ref_counted;
import :unique_ptr;    // Array<UniquePtr<T>> homogeneous container flavor (RegisterUniquePtrArrayType)
import :type_registry; // GlobalTypeRegistry().All() - the derived-type query (RTTI layer)
import :string;        // String attribute values (category/displayName) for the sort
// NB: reflection imports RTTI/base partitions ONLY. Capability like create-by-type
// flows IN through registration-time function pointers (createElement/canCreateElement), never a
// serialization import - that would half-close a partition cycle (serialization consumes reflection).

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------
namespace foundation::core::detail
{
    template <typename>
    struct MemberTraits;
    template <typename C, typename M>
    struct MemberTraits<M C::*>
    {
        using Class = C;
        using Member = M;
    };

    template <typename T, typename M, auto Member>
    Variant PropertyGet(const Instance& instance)
    {
        const T* object = static_cast<const T*>(instance.Pointer());
        return Variant::From<M>(object->*Member);
    }

    template <typename T, typename M, auto Member>
    Status PropertySet(const Instance& instance, const Variant& value)
    {
        T* object = static_cast<T*>(instance.Pointer());
        if constexpr (std::is_enum_v<M>)
        {
            // An enum property crosses from script as its underlying INT (script backends have no
            // native enum type; AngelScript enums are int-backed), so accept an i64/f64 and cast - as well as a
            // properly-typed enum Variant. M is known here, so the cast is well-defined.
            if (const M* typed = value.TryGet<M>())
            {
                object->*Member = *typed;
                return Status{};
            }
            if (const i64* i = value.TryGet<i64>())
            {
                object->*Member = static_cast<M>(*i);
                return Status{};
            }
            if (const f64* d = value.TryGet<f64>())
            {
                object->*Member = static_cast<M>(static_cast<i64>(*d));
                return Status{};
            }
            return Status{ErrorCode::InvalidArgument};
        }
        else
        {
            const M* typed = value.TryGet<M>();
            if (typed == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            object->*Member = *typed;
            return Status{};
        }
    }

    template <typename T, typename M, auto Member>
    void* PropertyAddress(const Instance& instance)
    {
        T* object = static_cast<T*>(instance.Pointer());
        return &(object->*Member);
    }

    // A COMPUTED (getter-backed) property: the value is derived by calling a const
    // zero-arg getter that returns by value, not read from a stored field. get marshals
    // the returned value through a Variant; set is not supported (read-only); there is no
    // field address. Emits as a parens-less getter in the script backends, exactly like a
    // stored property, but the value is computed on each read.
    template <typename Getter>
    struct GetterTraits;
    template <typename C, typename R>
    struct GetterTraits<R (C::*)() const>
    {
        using Class = C;
        using Return = R;
    };
    template <typename C, typename R>
    struct GetterTraits<R (C::*)() const noexcept>
    {
        using Class = C;
        using Return = R;
    };

    template <typename T, typename R, auto Getter>
    Variant GetterPropertyGet(const Instance& instance)
    {
        const T* object = static_cast<const T*>(instance.Pointer());
        return Variant::From<R>((object->*Getter)());
    }

    template <typename T, typename R, auto Getter>
    Status GetterPropertySet(const Instance&, const Variant&)
    {
        return Status{ErrorCode::NotSupported}; // a computed getter is read-only
    }

    // The setter half of an accessor property: a one-argument member taking the value by
    // value or const reference. The Variant must carry exactly R (no coercion: an accessor
    // guards an invariant, so it takes the type it declared).
    template <typename Setter>
    struct SetterTraits;
    template <typename C, typename A>
    struct SetterTraits<void (C::*)(A)>
    {
        using Class = C;
        using Argument = std::remove_cv_t<std::remove_reference_t<A>>;
    };
    template <typename C, typename A>
    struct SetterTraits<void (C::*)(A) noexcept>
    {
        using Class = C;
        using Argument = std::remove_cv_t<std::remove_reference_t<A>>;
    };

    template <typename T, typename R, auto Setter>
    Status SetterPropertySet(const Instance& instance, const Variant& value)
    {
        T* object = static_cast<T*>(instance.Pointer());
        if (const R* typed = value.TryGet<R>())
        {
            (object->*Setter)(*typed);
            return Status{};
        }
        return Status{ErrorCode::InvalidArgument};
    }

    // A NESTED (structure) property: the member is itself a reflected type the tooling should
    // recurse INTO, not a leaf value. This is the escape hatch for members that cannot marshal
    // through a Variant - e.g. non-copyable Object members (RefCounted deletes its copy ctor).
    // get returns an EMPTY Variant and set is unsupported (the value is never passed by copy);
    // `type` + `address` are populated so a consumer reaches the nested instance in place and
    // recurses into its properties. A POINTER member resolves `address` to the POINTEE (null when
    // the member is null - consumers must null-check before recursing).
    template <typename M>
    struct NestedPointee
    {
        using Type = M;
        static constexpr bool isPointer = false;
    };
    template <typename M>
    struct NestedPointee<M*>
    {
        using Type = M;
        static constexpr bool isPointer = true;
    };

    // The canonical reflected TypeInfo for a nested type: an intrusive Object exposes it as
    // StaticType() (the patched, property-carrying one); a value type (REFLECT_VALUE)
    // has no StaticType() and uses TypeOf<P>(). TypeOf<Object-type>() is a DIFFERENT, unpatched
    // TypeInfo, so the nested member's `type` must resolve through here.
    template <typename P>
    [[nodiscard]] const TypeInfo* NestedTypeInfo() noexcept
    {
        if constexpr (requires { P::StaticType(); })
        {
            return &P::StaticType();
        }
        else
        {
            return &TypeOf<P>();
        }
    }

    inline Variant NestedPropertyGet(const Instance&)
    {
        return Variant{}; // a nested structure is not read by value - recurse via address
    }
    inline Status NestedPropertySet(const Instance&, const Variant&)
    {
        return Status{ErrorCode::NotSupported}; // nor written by value
    }
    template <typename T, typename M, auto Member>
    void* NestedPropertyAddress(const Instance& instance)
    {
        T* object = static_cast<T*>(instance.Pointer());
        if constexpr (NestedPointee<M>::isPointer)
        {
            return static_cast<void*>(object->*Member); // the pointee (may be null)
        }
        else
        {
            return static_cast<void*>(&(object->*Member));
        }
    }

    [[nodiscard]] inline bool CStringEquals(const char* a, const char* b) noexcept
    {
        usize i = 0;
        while (a[i] != '\0' && a[i] == b[i])
        {
            ++i;
        }
        return a[i] == b[i];
    }
}

export namespace foundation::core
{
    enum class PropertyFlags : u32
    {
        None = 0,
        ReadOnly = 1u << 0,
        // The property is a nested reflected structure to recurse into (via `address`), not a
        // leaf value; `get` is empty and `set` unsupported. See TypeBuilder::Nested.
        Nested = 1u << 1,
    };

    struct PropertyInfo
    {
        const char* name;
        const TypeInfo* type;
        PropertyFlags flags;
        Variant (*get)(const Instance&);
        Status (*set)(const Instance&, const Variant&);
        // Raw address of the underlying field (null for computed properties). The type-erased
        // escape hatch for generic tooling: a Variant of an arbitrary reflected type (e.g. an
        // enum known only by TypeInfo) cannot be CONSTRUCTED at runtime, so editors/binding
        // generators read and write such fields in place through this instead.
        void* (*address)(const Instance&) = nullptr;
        // Per-PROPERTY attributes (tooling metadata: value ranges, conditional visibility, ...)
        // - attached fluently via TypeBuilder::PropAttribute after Property().
        const Attribute* attributes = nullptr;
        u32 attributeCount = 0;
    };

    [[nodiscard]] inline Variant GetProperty(const PropertyInfo& property, const Instance& instance)
    {
        return property.get(instance);
    }

    /// Write a property. A read-only one refuses (NotSupported) whoever asks - a script
    /// backend, an editor, an agent's tool - so the flag is a contract, not a hint each
    /// consumer re-checks; the raw address stays the deliberate way around it.
    [[nodiscard]] inline Status SetProperty(const PropertyInfo& property, const Instance& instance,
                                            const Variant& value)
    {
        if ((static_cast<u32>(property.flags) & static_cast<u32>(PropertyFlags::ReadOnly)) != 0)
        {
            return Status{ErrorCode::NotSupported};
        }
        return property.set(instance, value);
    }

    // A nested structure property: recurse into `property.type`'s own properties using a
    // sub-Instance built from `property.address(instance)` (null-check it first - a null pointer
    // member yields a null address), rather than reading it as a leaf value. See TypeBuilder::Nested.
    [[nodiscard]] inline bool IsNested(const PropertyInfo& property) noexcept
    {
        return (static_cast<u32>(property.flags) & static_cast<u32>(PropertyFlags::Nested)) != 0;
    }

    // Properties declared directly on `type` (not inherited).
    [[nodiscard]] inline Span<const PropertyInfo> Properties(const TypeInfo& type) noexcept
    {
        return Span<const PropertyInfo>{type.properties, type.propertyCount};
    }

    // Count / by-index access (own properties only) for binding generators that
    // enumerate rather than search.
    [[nodiscard]] inline usize PropertyCount(const TypeInfo& type) noexcept
    {
        return type.propertyCount;
    }
    [[nodiscard]] inline const PropertyInfo& PropertyAt(const TypeInfo& type, usize index) noexcept
    {
        DIAGNOSTIC_ASSERT(index < type.propertyCount);
        return type.properties[index];
    }

    // Searches `type` and its base chain for a property by name.
    [[nodiscard]] inline const PropertyInfo* FindProperty(const TypeInfo& type,
                                                          const char* name) noexcept
    {
        for (const TypeInfo* t = &type; t != nullptr; t = t->base)
        {
            for (u32 i = 0; i < t->propertyCount; ++i)
            {
                if (detail::CStringEquals(t->properties[i].name, name))
                {
                    return &t->properties[i];
                }
            }
        }
        return nullptr;
    }

    // =======================================================================
    // Methods - instance, const, and static, via Variant args.
    // =======================================================================
    struct ParamInfo
    {
        // Resolved lazily (a getter, not a pointer) so a type can reflect methods
        // that reference its own type without a recursive static-init.
        const TypeInfo* (*type)();
        const char* name; // optional; "" when unknown
    };

    /// The operator a reflected method implements, for consumers that spell operators (the
    /// scripting backends: AngelScript's opAdd, Luau's __add). Opaque authored metadata like
    /// overloadedName: the method's left operand is its first parameter (a static method); a
    /// unary operator takes its one operand. Only what the C++ type defines is marked.
    enum class MethodOperator : u8
    {
        None,
        Add,
        Subtract,
        Multiply,
        Divide,
        Negate,
        Equals,
    };

    struct MethodInfo
    {
        const char* name;
        const TypeInfo* (*returnType)(); // returns nullptr for void; lazy (see ParamInfo)
        const ParamInfo* params;
        u32 paramCount;
        bool isStatic;
        bool isConst;
        Result<Variant> (*invoke)(const Instance&, Span<Variant>);
        // An OPTIONAL distinct name for a member of an overload set. C++ overloading (same name,
        // different signatures) cannot be disambiguated by a consumer that binds one entry per name
        // (the scripting backends), so an overload set gives each member a distinct name here.
        // Opaque authored metadata - Core stores it and does not interpret it (like param names);
        // the scripting layer decides how it is spelled and enforces uniqueness. Empty/null = none.
        const char* overloadedName = nullptr;
        // The operator this method implements (MethodOperator), None for a plain method.
        MethodOperator op = MethodOperator::None;
    };

    [[nodiscard]] inline Result<Variant> InvokeMethod(const MethodInfo& method,
                                                      const Instance& instance, Span<Variant> args)
    {
        return method.invoke(instance, args);
    }

    // Convenience for static methods (no target object).
    [[nodiscard]] inline Result<Variant> InvokeStatic(const MethodInfo& method, Span<Variant> args)
    {
        return method.invoke(Instance{}, args);
    }

    [[nodiscard]] inline Span<const MethodInfo> Methods(const TypeInfo& type) noexcept
    {
        return Span<const MethodInfo>{type.methods, type.methodCount};
    }

    [[nodiscard]] inline usize MethodCount(const TypeInfo& type) noexcept
    {
        return type.methodCount;
    }
    [[nodiscard]] inline const MethodInfo& MethodAt(const TypeInfo& type, usize index) noexcept
    {
        DIAGNOSTIC_ASSERT(index < type.methodCount);
        return type.methods[index];
    }

    // A method's parameters by count / index (for binding each overload's signature).
    [[nodiscard]] inline usize ParamCount(const MethodInfo& method) noexcept
    {
        return method.paramCount;
    }
    [[nodiscard]] inline const ParamInfo& ParamAt(const MethodInfo& method, usize index) noexcept
    {
        DIAGNOSTIC_ASSERT(index < method.paramCount);
        return method.params[index];
    }

    // =======================================================================
    // Constructors - let scripting instantiate a type. invoke() validates its
    // args and returns the new instance as a Variant (a value, or object mode
    // for Object-derived types). A type may have several (overloads).
    // =======================================================================
    struct ConstructorInfo
    {
        const ParamInfo* params;
        u32 paramCount;
        Result<Variant> (*invoke)(Span<Variant> args);
    };

    [[nodiscard]] inline Span<const ConstructorInfo> Constructors(const TypeInfo& type) noexcept
    {
        return Span<const ConstructorInfo>{type.constructors, type.constructorCount};
    }

    [[nodiscard]] inline usize ConstructorCount(const TypeInfo& type) noexcept
    {
        return type.constructorCount;
    }
    [[nodiscard]] inline const ConstructorInfo& ConstructorAt(const TypeInfo& type,
                                                              usize index) noexcept
    {
        DIAGNOSTIC_ASSERT(index < type.constructorCount);
        return type.constructors[index];
    }

    // Borrows an Instance over the value/object a Variant owns (for binding
    // layers that call properties/methods on a reflected Variant).
    [[nodiscard]] inline Instance ToInstance(Variant& value) noexcept
    {
        if (value.IsResolving())
        {
            // Resolve mode: recompute the live address every time (the owning layer's resolver does
            // the O(1) lookup). A dead entity / removed component resolves to null -> empty Instance,
            // so the caller (a script get/set/call) fails cleanly - never a stale/dangling address.
            void* address = value.Resolve();
            return (address != nullptr) ? Instance(address, value.Type()) : Instance{};
        }
        if (value.IsBorrow())
        {
            // A borrow points into a parent's storage; a structural mutation since capture may have
            // freed/moved it. Revalidate against the mutation generation - stale => empty Instance, so
            // the caller (a script get/set) fails cleanly instead of dereferencing a dangling address.
            return value.BorrowValid() ? Instance(value.BorrowAddress(), value.Type()) : Instance{};
        }
        if (value.IsObject())
        {
            return Instance(value.AsObject(), value.Type());
        }
        return Instance(value.ValuePointer(), value.Type());
    }

    // Constructs an instance by picking the constructor whose arity matches and
    // whose argument types accept `args`. Returns InvalidArgument if none match.
    [[nodiscard]] inline Result<Variant> Construct(const TypeInfo& type, Span<Variant> args)
    {
        for (u32 i = 0; i < type.constructorCount; ++i)
        {
            const ConstructorInfo& ctor = type.constructors[i];
            if (ctor.paramCount != args.Size())
            {
                continue;
            }
            Result<Variant> result = ctor.invoke(args);
            if (result.HasValue())
            {
                return result;
            }
        }
        return Err(ErrorCode::InvalidArgument);
    }

    [[nodiscard]] inline const MethodInfo* FindMethod(const TypeInfo& type,
                                                      const char* name) noexcept
    {
        for (const TypeInfo* t = &type; t != nullptr; t = t->base)
        {
            for (u32 i = 0; i < t->methodCount; ++i)
            {
                if (detail::CStringEquals(t->methods[i].name, name))
                {
                    return &t->methods[i];
                }
            }
        }
        return nullptr;
    }

    // Overload-aware lookup: matches name + exact parameter types (using the
    // ParamInfo type info each method carries). Lets several same-named methods
    // coexist and be resolved by signature.
    [[nodiscard]] inline const MethodInfo*
    FindMethod(const TypeInfo& type, const char* name,
               Span<const TypeInfo* const> paramTypes) noexcept
    {
        for (const TypeInfo* t = &type; t != nullptr; t = t->base)
        {
            for (u32 i = 0; i < t->methodCount; ++i)
            {
                const MethodInfo& method = t->methods[i];
                if (!detail::CStringEquals(method.name, name))
                {
                    continue;
                }
                if (method.paramCount != paramTypes.Size())
                {
                    continue;
                }
                bool match = true;
                for (u32 p = 0; p < method.paramCount; ++p)
                {
                    if (method.params[p].type() != paramTypes[p])
                    {
                        match = false;
                        break;
                    }
                }
                if (match)
                {
                    return &method;
                }
            }
        }
        return nullptr;
    }

    // =======================================================================
    // Attributes - freeform key -> Variant metadata on a type.
    // =======================================================================
    struct Attribute
    {
        const char* key;
        Variant value;
    };

    [[nodiscard]] inline Span<const Attribute> Attributes(const PropertyInfo& property) noexcept
    {
        return Span<const Attribute>{property.attributes, property.attributeCount};
    }

    [[nodiscard]] inline const Attribute* FindAttribute(const PropertyInfo& property,
                                                        StringView key) noexcept
    {
        for (u32 i = 0; i < property.attributeCount; ++i)
        {
            if (StringView(reinterpret_cast<const utf8char*>(property.attributes[i].key)) == key)
            {
                return &property.attributes[i];
            }
        }
        return nullptr;
    }

    [[nodiscard]] inline Span<const Attribute> Attributes(const TypeInfo& type) noexcept
    {
        return Span<const Attribute>{type.attributes, type.attributeCount};
    }

    [[nodiscard]] inline usize AttributeCount(const TypeInfo& type) noexcept
    {
        return type.attributeCount;
    }
    [[nodiscard]] inline const Attribute& AttributeAt(const TypeInfo& type, usize index) noexcept
    {
        DIAGNOSTIC_ASSERT(index < type.attributeCount);
        return type.attributes[index];
    }

    [[nodiscard]] inline const Variant* FindAttribute(const TypeInfo& type,
                                                      const char* key) noexcept
    {
        for (u32 i = 0; i < type.attributeCount; ++i)
        {
            if (detail::CStringEquals(type.attributes[i].key, key))
            {
                return &type.attributes[i].value;
            }
        }
        return nullptr;
    }

    // =======================================================================
    // Constants - named static values exposed for scripting (e.g. Float3::Zero,
    // Quaternion::Identity, Guid::Nil). Each holds its value as a Variant.
    // =======================================================================
    struct ConstantInfo
    {
        const char* name;
        const TypeInfo* type;
        Variant value;
    };

    [[nodiscard]] inline Span<const ConstantInfo> Constants(const TypeInfo& type) noexcept
    {
        return Span<const ConstantInfo>{type.constants, type.constantCount};
    }

    [[nodiscard]] inline usize ConstantCount(const TypeInfo& type) noexcept
    {
        return type.constantCount;
    }
    [[nodiscard]] inline const ConstantInfo& ConstantAt(const TypeInfo& type, usize index) noexcept
    {
        DIAGNOSTIC_ASSERT(index < type.constantCount);
        return type.constants[index];
    }

    [[nodiscard]] inline const ConstantInfo* FindConstant(const TypeInfo& type,
                                                          const char* name) noexcept
    {
        for (u32 i = 0; i < type.constantCount; ++i)
        {
            if (detail::CStringEquals(type.constants[i].name, name))
            {
                return &type.constants[i];
            }
        }
        return nullptr;
    }

    // =======================================================================
    // Container reflection - generic indexed access to Array<T>, so
    // tools/scripting can iterate without knowing the element type statically.
    // =======================================================================
    enum class ContainerFlags : u32
    {
        None = 0,
        // Elements are polymorphic (Array<RefPtr<Base>>): getAt returns an object-mode Variant
        // whose Type() is the ELEMENT's dynamic type; elementType is the static Base. Consumers
        // recurse into the concrete type. setAt is unsupported (mutation is a separate design).
        PolymorphicElements = 1u << 0,
    };

    struct ContainerInfo
    {
        const TypeInfo* elementType;
        usize (*size)(const Instance&);
        Variant (*getAt)(const Instance&, usize index);
        Status (*setAt)(const Instance&, usize index, const Variant& value);
        ContainerFlags flags = ContainerFlags::None;
        // Mutation ops (Variant-free - RefPtr elements are non-copyable, so everything is by
        // Instance/address). Null when the flavor does not support the op; callers null-check.
        // - createElement: POLYMORPHIC only - create `concrete` at `index` via the registrant's
        //   factory (reflection defines + calls this slot but never imports the factory), return the
        //   new element's Instance (dynamic type) for editing; empty on failure / read-only container.
        // - canCreateElement: whether `concrete` is creatable here - backs the add-dropdown eligibility
        //   filter so the derived-type query (EnumerateDerived) stays pure RTTI.
        // - emplaceDefault: HOMOGENEOUS Array<T>/BoundedArray - default-construct at `index`.
        // - removeAt / moveElement: both flavors (module arrays are order-sensitive, so move matters).
        Instance (*createElement)(const Instance&, usize index, const TypeInfo& concrete) = nullptr;
        bool (*canCreateElement)(const TypeInfo& concrete) = nullptr;
        Instance (*emplaceDefault)(const Instance&, usize index) = nullptr;
        Status (*removeAt)(const Instance&, usize index) = nullptr;
        Status (*moveElement)(const Instance&, usize from, usize to) = nullptr;
        // Address-based element access (mirrors the Nested mechanism): returns a borrowed Instance
        // { pointee, elementType } so a consumer can descend into a non-copyable / non-Object element
        // (e.g. Array<UniquePtr<T>> of a plain reflected value) the way getAt cannot - getAt yields a
        // Variant, which only carries copyable values or RefPtr<Object>. Object containers may fill it
        // too (dynamic type), giving both flavors one uniform descent path. Null = no address access.
        Instance (*addressAt)(const Instance&, usize index) = nullptr;
    };

    [[nodiscard]] inline bool IsContainer(const TypeInfo& type) noexcept
    {
        return type.container != nullptr;
    }

    [[nodiscard]] inline bool IsPolymorphicContainer(const ContainerInfo& container) noexcept
    {
        return (static_cast<u32>(container.flags) &
                static_cast<u32>(ContainerFlags::PolymorphicElements)) != 0;
    }

    [[nodiscard]] inline usize ContainerSize(const ContainerInfo& container,
                                             const Instance& instance)
    {
        return container.size(instance);
    }

    [[nodiscard]] inline Variant ContainerGetAt(const ContainerInfo& container,
                                                const Instance& instance, usize index)
    {
        return container.getAt(instance, index);
    }

    inline Status ContainerSetAt(const ContainerInfo& container, const Instance& instance,
                                 usize index, const Variant& value)
    {
        return container.setAt(instance, index, value);
    }

    // Mutation (null-op-safe). create/emplace return the new element's Instance (empty on failure);
    // remove/move return NotSupported when the container flavor does not offer the op.
    [[nodiscard]] inline Instance ContainerCreateElement(const ContainerInfo& container,
                                                         const Instance& instance, usize index,
                                                         const TypeInfo& concrete)
    {
        if (container.createElement == nullptr)
        {
            return Instance{};
        }
        const Instance created = container.createElement(instance, index, concrete);
        BumpReflectionMutationGeneration(); // structural: invalidates any live borrows into this array
        return created;
    }
    [[nodiscard]] inline bool ContainerCanCreateElement(const ContainerInfo& container,
                                                        const TypeInfo& concrete)
    {
        return container.canCreateElement != nullptr && container.canCreateElement(concrete);
    }
    [[nodiscard]] inline Instance ContainerEmplaceDefault(const ContainerInfo& container,
                                                          const Instance& instance, usize index)
    {
        if (container.emplaceDefault == nullptr)
        {
            return Instance{};
        }
        const Instance created = container.emplaceDefault(instance, index);
        BumpReflectionMutationGeneration(); // structural
        return created;
    }
    inline Status ContainerRemoveAt(const ContainerInfo& container, const Instance& instance,
                                    usize index)
    {
        if (container.removeAt == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }
        const Status status = container.removeAt(instance, index);
        BumpReflectionMutationGeneration(); // structural
        return status;
    }
    inline Status ContainerMoveElement(const ContainerInfo& container, const Instance& instance,
                                       usize from, usize to)
    {
        if (container.moveElement == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }
        const Status status = container.moveElement(instance, from, to);
        BumpReflectionMutationGeneration(); // structural (element addresses may shift)
        return status;
    }
    // Borrowed Instance for element `index` (empty when the flavor offers no address access, or the
    // element is null / out of range). Consumers recurse into it via the element type's properties.
    [[nodiscard]] inline Instance ContainerAddressAt(const ContainerInfo& container,
                                                     const Instance& instance, usize index)
    {
        return container.addressAt != nullptr ? container.addressAt(instance, index) : Instance{};
    }

    // A type attribute's String value (e.g. "category" / "displayName"), or a fallback.
    [[nodiscard]] inline StringView TypeAttrString(const TypeInfo& type, const char* key,
                                                   StringView fallback) noexcept
    {
        const Variant* v = FindAttribute(type, key);
        const String* s = (v != nullptr) ? v->TryGet<String>() : nullptr;
        return (s != nullptr) ? s->AsView() : fallback;
    }

    // The registered types that derive from `base` (excluding `base` itself), sorted by category
    // then displayName for a stable UI order. PURE RTTI - it does not judge creatability; a consumer
    // (the add-module dropdown) filters eligibility through the container's canCreateElement, which
    // keeps this query free of any serialization/factory dependency.
    inline void EnumerateDerived(const TypeInfo& base, Array<const TypeInfo*>& out)
    {
        for (const TypeInfo* t : GlobalTypeRegistry().All())
        {
            if (t == &base)
            {
                continue;
            }
            for (const TypeInfo* b = t->base; b != nullptr; b = b->base)
            {
                if (b == &base)
                {
                    out.PushBack(t);
                    break;
                }
            }
        }
        // Insertion sort by (category, displayName) - stable, N is small.
        auto cmp = [](StringView a, StringView b) noexcept -> int
        {
            const usize n = a.Size() < b.Size() ? a.Size() : b.Size();
            for (usize i = 0; i < n; ++i)
            {
                if (a[i] != b[i])
                {
                    return a[i] < b[i] ? -1 : 1;
                }
            }
            return a.Size() == b.Size() ? 0 : (a.Size() < b.Size() ? -1 : 1);
        };
        auto less = [&cmp](const TypeInfo* a, const TypeInfo* b) noexcept -> bool
        {
            const StringView an(reinterpret_cast<const utf8char*>(a->name));
            const StringView bn(reinterpret_cast<const utf8char*>(b->name));
            const int c = cmp(TypeAttrString(*a, "category", StringView{}),
                              TypeAttrString(*b, "category", StringView{}));
            return c != 0 ? c < 0
                          : cmp(TypeAttrString(*a, "displayName", an),
                                TypeAttrString(*b, "displayName", bn)) < 0;
        };
        for (usize i = 1; i < out.Size(); ++i)
        {
            const TypeInfo* key = out[i];
            usize j = i;
            while (j > 0 && less(key, out[j - 1]))
            {
                out[j] = out[j - 1];
                --j;
            }
            out[j] = key;
        }
    }

    // Element move by adjacent swaps (order-preserving, works for move-only elements).
    template <typename Indexable>
    void BubbleMove(Indexable& seq, usize from, usize to)
    {
        while (from < to)
        {
            auto tmp = Move(seq[from]);
            seq[from] = Move(seq[from + 1]);
            seq[from + 1] = Move(tmp);
            ++from;
        }
        while (from > to)
        {
            auto tmp = Move(seq[from]);
            seq[from] = Move(seq[from - 1]);
            seq[from - 1] = Move(tmp);
            --from;
        }
    }

    // Registers Array<T> as a reflected container (patches TypeOf<Array<T>>()). Homogeneous value
    // elements: setAt writes by Variant; emplaceDefault inserts a default T; remove/move reorder.
    template <typename T>
    void RegisterArrayType()
    {
        using Arr = Array<T>;
        static const ContainerInfo info{
            .elementType = &TypeOf<T>(),
            .size = [](const Instance& i) -> usize
            { return static_cast<const Arr*>(i.Pointer())->Size(); },
            .getAt = [](const Instance& i, usize index) -> Variant
            { return Variant::From<T>((*static_cast<const Arr*>(i.Pointer()))[index]); },
            .setAt =
                [](const Instance& i, usize index, const Variant& value) -> Status
            {
                const T* typed = value.TryGet<T>();
                if (typed == nullptr)
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                (*static_cast<Arr*>(i.Pointer()))[index] = *typed;
                return Status{};
            },
            .flags = ContainerFlags::None,
            .createElement = nullptr, // homogeneous: the element type is fixed - use emplaceDefault
            .canCreateElement = nullptr,
            .emplaceDefault =
                [](const Instance& i, usize index) -> Instance
            {
                Arr* a = static_cast<Arr*>(i.Pointer());
                if (index > a->Size())
                {
                    return Instance{};
                }
                T& ref = a->Insert(index, T{});
                return Instance(&ref, &TypeOf<T>());
            },
            .removeAt =
                [](const Instance& i, usize index) -> Status
            {
                Arr* a = static_cast<Arr*>(i.Pointer());
                if (index >= a->Size())
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                a->RemoveAt(index);
                return Status{};
            },
            .moveElement =
                [](const Instance& i, usize from, usize to) -> Status
            {
                Arr* a = static_cast<Arr*>(i.Pointer());
                if (from >= a->Size() || to >= a->Size())
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                BubbleMove(*a, from, to);
                return Status{};
            },
            .addressAt = [](const Instance& i, usize index) -> Instance
            {
                Arr* a = static_cast<Arr*>(i.Pointer());
                if (index >= a->Size())
                {
                    return Instance{};
                }
                return Instance(&(*a)[index], &TypeOf<T>()); // in-place element (edit through it)
            }};
        const_cast<TypeInfo&>(TypeOf<Arr>()).container = &info;
    }

    // Per-Base holder for the create-by-type factory a registrant supplies (namespace-scope static,
    // so the captureless container slots can reach it). Read-only when left null.
    template <typename Base>
    struct PolymorphicElementFactory
    {
        static inline RefPtr<Base> (*create)(const TypeInfo&) = nullptr;
        static inline bool (*canCreate)(const TypeInfo&) = nullptr;
    };

    // Registers Array<RefPtr<Base>> as a POLYMORPHIC reflected container: getAt derefs the RefPtr
    // and returns an object-mode Variant whose Type() is the ELEMENT's dynamic type (so a consumer
    // recurses into the concrete derived type's properties). elementType stays the static Base; a
    // null element yields an empty Variant (consumers null-check). setAt is unsupported (elements
    // are non-copyable). The create-by-type FACTORY is passed IN (createElement/canCreate) - reflection
    // never imports it; omit both for a cleanly read-only container. The particles impl unit passes
    // the standard serialization adapter.
    template <typename Base>
    void RegisterPolymorphicArrayType(RefPtr<Base> (*create)(const TypeInfo&) = nullptr,
                                      bool (*canCreate)(const TypeInfo&) = nullptr)
    {
        using Arr = Array<RefPtr<Base>>;
        using Factory = PolymorphicElementFactory<Base>;
        Factory::create = create;
        Factory::canCreate = canCreate;
        static const ContainerInfo info{
            .elementType = &Base::StaticType(),
            .size = [](const Instance& i) -> usize
            { return static_cast<const Arr*>(i.Pointer())->Size(); },
            .getAt =
                [](const Instance& i, usize index) -> Variant
            {
                const RefPtr<Base>& elem = (*static_cast<const Arr*>(i.Pointer()))[index];
                return elem.Get() != nullptr ? Variant::From(elem) : Variant{};
            },
            .setAt = [](const Instance&, usize, const Variant&) -> Status
            { return Status{ErrorCode::NotSupported}; },
            .flags = ContainerFlags::PolymorphicElements,
            .createElement =
                [](const Instance& i, usize index, const TypeInfo& concrete) -> Instance
            {
                if (Factory::create == nullptr) // read-only container (no factory supplied)
                {
                    return Instance{};
                }
                Arr* a = static_cast<Arr*>(i.Pointer());
                if (index > a->Size())
                {
                    return Instance{};
                }
                RefPtr<Base> obj = Factory::create(concrete); // null if not creatable / not a Base
                if (obj.Get() == nullptr)
                {
                    return Instance{};
                }
                Base* raw = obj.Get();
                a->Insert(index, Move(obj));
                return Instance(raw, raw->GetType());
            },
            .canCreateElement = [](const TypeInfo& concrete) -> bool
            { return Factory::canCreate != nullptr && Factory::canCreate(concrete); },
            .emplaceDefault = nullptr, // polymorphic: no default concrete type - use createElement
            .removeAt =
                [](const Instance& i, usize index) -> Status
            {
                Arr* a = static_cast<Arr*>(i.Pointer());
                if (index >= a->Size())
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                a->RemoveAt(index);
                return Status{};
            },
            .moveElement =
                [](const Instance& i, usize from, usize to) -> Status
            {
                Arr* a = static_cast<Arr*>(i.Pointer());
                if (from >= a->Size() || to >= a->Size())
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                BubbleMove(*a, from, to);
                return Status{};
            },
            .addressAt = [](const Instance& i, usize index) -> Instance
            {
                const Arr* a = static_cast<const Arr*>(i.Pointer());
                if (index >= a->Size())
                {
                    return Instance{};
                }
                Base* raw = (*a)[index].Get();
                return raw != nullptr ? Instance(raw, raw->GetType()) : Instance{};
            }};
        const_cast<TypeInfo&>(TypeOf<Arr>()).container = &info;
    }

    // Registers Array<UniquePtr<T>> as a HOMOGENEOUS reflected container of a plain (non-Object,
    // possibly non-copyable) reflected value `T`. getAt yields an empty Variant - like Nested, the
    // element is reached by ADDRESS (addressAt returns a borrowed Instance{ pointee, TypeOf<T> }) so a
    // consumer recurses into T's properties without copying. setAt is unsupported (owned, non-copyable);
    // emplaceDefault default-constructs a T when T is default-constructible; remove/move reorder.
    template <typename T>
    void RegisterUniquePtrArrayType()
    {
        using Arr = Array<UniquePtr<T>>;
        static const ContainerInfo info{
            .elementType = &TypeOf<T>(),
            .size = [](const Instance& i) -> usize
            { return static_cast<const Arr*>(i.Pointer())->Size(); },
            .getAt = [](const Instance&, usize) -> Variant { return Variant{}; },
            .setAt = [](const Instance&, usize, const Variant&) -> Status
            { return Status{ErrorCode::NotSupported}; },
            .flags = ContainerFlags::None,
            .createElement = nullptr,
            .canCreateElement = nullptr,
            .emplaceDefault =
                [](const Instance& i, usize index) -> Instance
            {
                if constexpr (requires { T{}; })
                {
                    Arr* a = static_cast<Arr*>(i.Pointer());
                    if (index > a->Size())
                    {
                        return Instance{};
                    }
                    UniquePtr<T>& slot = a->Insert(index, MakeUnique<T>(DefaultAllocator()));
                    return Instance(slot.Get(), &TypeOf<T>());
                }
                else
                {
                    return Instance{};
                }
            },
            .removeAt =
                [](const Instance& i, usize index) -> Status
            {
                Arr* a = static_cast<Arr*>(i.Pointer());
                if (index >= a->Size())
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                a->RemoveAt(index);
                return Status{};
            },
            .moveElement =
                [](const Instance& i, usize from, usize to) -> Status
            {
                Arr* a = static_cast<Arr*>(i.Pointer());
                if (from >= a->Size() || to >= a->Size())
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                BubbleMove(*a, from, to);
                return Status{};
            },
            .addressAt = [](const Instance& i, usize index) -> Instance
            {
                const Arr* a = static_cast<const Arr*>(i.Pointer());
                if (index >= a->Size())
                {
                    return Instance{};
                }
                T* raw = (*a)[index].Get();
                return raw != nullptr ? Instance(raw, &TypeOf<T>()) : Instance{};
            }};
        const_cast<TypeInfo&>(TypeOf<Arr>()).container = &info;
    }
}

namespace foundation::core::detail
{
    // ---- bounded-array (count-bound inline vector) container support --------------------------
    // A `T member[N]` C-array paired with a live `count` member (the logical length) is reflected
    // as a container whose SIZE is the count clamped to [0, N] - so garbage slots beyond the count
    // never surface. The container operates on the OWNER instance (it needs both the array and the
    // count), so the BoundedArray property's `address` returns the owner identity.
    template <typename M>
    struct CArrayTraits;
    template <typename C, typename T, usize N>
    struct CArrayTraits<T (C::*)[N]>
    {
        using Owner = C;
        using Element = T;
        static constexpr usize Extent = N;
    };

    template <typename Owner, typename Elem, usize N, auto ArrayMember, auto CountMember>
    usize BoundedArraySize(const Instance& inst)
    {
        const Owner* owner = static_cast<const Owner*>(inst.Pointer());
        const auto raw = owner->*CountMember;
        if (raw <= 0)
        {
            return 0;
        }
        const usize c = static_cast<usize>(raw);
        return c < N ? c : N; // clamp corrupt/oversized counts to the capacity
    }
    template <typename Owner, typename Elem, usize N, auto ArrayMember, auto CountMember>
    Variant BoundedArrayGetAt(const Instance& inst, usize index)
    {
        const Owner* owner = static_cast<const Owner*>(inst.Pointer());
        return Variant::From<Elem>((owner->*ArrayMember)[index]);
    }
    template <typename Owner, typename Elem, usize N, auto ArrayMember, auto CountMember>
    Status BoundedArraySetAt(const Instance& inst, usize index, const Variant& value)
    {
        const Elem* typed = value.TryGet<Elem>();
        if (typed == nullptr)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        Owner* owner = static_cast<Owner*>(inst.Pointer());
        (owner->*ArrayMember)[index] = *typed;
        return Status{};
    }
    inline void* BoundedArrayAddress(const Instance& inst)
    {
        return inst.Pointer(); // the owner - the container reads array + count off it
    }

    template <typename Owner, typename Elem, usize N, auto ArrayMember, auto CountMember>
    usize BoundedClampedCount(Owner* owner)
    {
        const auto raw = owner->*CountMember;
        if (raw <= 0)
        {
            return 0;
        }
        const usize c = static_cast<usize>(raw);
        return c < N ? c : N;
    }
    template <typename Owner, typename Elem, usize N, auto ArrayMember, auto CountMember>
    Instance BoundedEmplaceDefault(const Instance& inst, usize index)
    {
        Owner* owner = static_cast<Owner*>(inst.Pointer());
        auto& count = owner->*CountMember;
        const usize c = BoundedClampedCount<Owner, Elem, N, ArrayMember, CountMember>(owner);
        if (c >= N || index > c)
        {
            return Instance{}; // full, or index past the end
        }
        auto& arr = owner->*ArrayMember;
        for (usize k = c; k > index; --k)
        {
            arr[k] = Move(arr[k - 1]); // shift up to open a slot
        }
        arr[index] = Elem{};
        count = static_cast<std::remove_reference_t<decltype(count)>>(c + 1);
        return Instance(&arr[index], &TypeOf<Elem>());
    }
    template <typename Owner, typename Elem, usize N, auto ArrayMember, auto CountMember>
    Status BoundedRemoveAt(const Instance& inst, usize index)
    {
        Owner* owner = static_cast<Owner*>(inst.Pointer());
        auto& count = owner->*CountMember;
        const usize c = BoundedClampedCount<Owner, Elem, N, ArrayMember, CountMember>(owner);
        if (index >= c)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        auto& arr = owner->*ArrayMember;
        for (usize k = index; k + 1 < c; ++k)
        {
            arr[k] = Move(arr[k + 1]); // shift down over the removed slot
        }
        count = static_cast<std::remove_reference_t<decltype(count)>>(c - 1);
        return Status{};
    }
    template <typename Owner, typename Elem, usize N, auto ArrayMember, auto CountMember>
    Status BoundedMove(const Instance& inst, usize from, usize to)
    {
        Owner* owner = static_cast<Owner*>(inst.Pointer());
        const usize c = BoundedClampedCount<Owner, Elem, N, ArrayMember, CountMember>(owner);
        if (from >= c || to >= c)
        {
            return Status{ErrorCode::InvalidArgument};
        }
        BubbleMove(owner->*ArrayMember, from, to);
        return Status{};
    }

    // Object-argument support: a parameter A may be a value type, or an object
    // form (RefPtr<U>, U*, or U&/const U& with U deriving Object). Object args
    // are extracted from an object-mode Variant via AsObject<U>().
    template <typename T>
    struct ArgRefPtr
    {
        static constexpr bool value = false;
    };
    template <typename U>
    struct ArgRefPtr<RefPtr<U>>
    {
        static constexpr bool value = true;
        using Pointee = U;
    };

    // An Array<E> parameter: a script hands it a list (an AngelScript array, a Luau table), which
    // its backend carries as an Array<Variant> of elements already narrowed to E's type; the call
    // converts it element by element. An Array<E> Variant (a native caller) is taken as it is.
    template <typename T>
    struct ArgArray
    {
        static constexpr bool value = false;
    };
    template <typename E>
    struct ArgArray<Array<E>>
    {
        static constexpr bool value = !std::is_same_v<E, Variant>;
        using Element = E;
    };

    template <typename A>
    [[nodiscard]] bool AcceptArg(const Variant& v) noexcept;
    template <typename A>
    [[nodiscard]] decltype(auto) ConvertArg(Variant& v) noexcept;

    template <typename A>
    [[nodiscard]] const TypeInfo* ParamTypeOf() noexcept
    {
        using Bare = std::remove_cvref_t<A>;
        if constexpr (ArgRefPtr<Bare>::value)
        {
            return &ArgRefPtr<Bare>::Pointee::StaticType();
        }
        else if constexpr (std::is_pointer_v<Bare> &&
                           std::is_base_of_v<Object, std::remove_cv_t<std::remove_pointer_t<Bare>>>)
        {
            return &std::remove_cv_t<std::remove_pointer_t<Bare>>::StaticType();
        }
        else if constexpr (std::is_class_v<Bare> && std::is_base_of_v<Object, Bare>)
        {
            return &Bare::StaticType();
        }
        else
        {
            return &TypeOf<Bare>();
        }
    }

    template <typename A>
    [[nodiscard]] bool AcceptArg(const Variant& v) noexcept
    {
        using Bare = std::remove_cvref_t<A>;
        if constexpr (std::is_same_v<Bare, Variant>)
        {
            (void)v;
            return true; // a Variant parameter is the generic sink: it accepts ANY argument as-is
        }
        else if constexpr (ArgRefPtr<Bare>::value)
        {
            using U = typename ArgRefPtr<Bare>::Pointee;
            return v.IsObject() && (v.AsObject() == nullptr || v.AsObject<U>() != nullptr);
        }
        else if constexpr (std::is_pointer_v<Bare> &&
                           std::is_base_of_v<Object, std::remove_cv_t<std::remove_pointer_t<Bare>>>)
        {
            using U = std::remove_cv_t<std::remove_pointer_t<Bare>>;
            return v.IsObject() && (v.AsObject() == nullptr || v.AsObject<U>() != nullptr);
        }
        else if constexpr (std::is_class_v<Bare> && std::is_base_of_v<Object, Bare>)
        {
            return v.IsObject() && v.AsObject<Bare>() != nullptr; // reference: must be non-null
        }
        else if constexpr (ArgArray<Bare>::value)
        {
            if (v.TryGet<Bare>() != nullptr)
            {
                return true;
            }
            const Array<Variant>* list = v.TryGet<Array<Variant>>();
            if (list == nullptr)
            {
                return false;
            }
            for (const Variant& element : *list)
            {
                if (!AcceptArg<typename ArgArray<Bare>::Element>(element))
                {
                    return false;
                }
            }
            return true;
        }
        else if constexpr (std::is_enum_v<Bare>)
        {
            // An enum arg is its underlying int (i64/f64) or a properly-typed enum Variant - see
            // ConvertArg. Accept all three so an overload check does not reject a script enum.
            return v.TryGet<Bare>() != nullptr || v.TryGet<i64>() != nullptr ||
                   v.TryGet<f64>() != nullptr;
        }
        else
        {
            return v.TryGet<Bare>() != nullptr;
        }
    }

    template <typename A>
    [[nodiscard]] decltype(auto) ConvertArg(Variant& v) noexcept
    {
        using Bare = std::remove_cvref_t<A>;
        if constexpr (std::is_same_v<Bare, Variant>)
        {
            return (v); // pass the Variant through unchanged (the generic payload sink)
        }
        else if constexpr (ArgRefPtr<Bare>::value)
        {
            using U = typename ArgRefPtr<Bare>::Pointee;
            return RefPtr<U>(v.AsObject<U>());
        }
        else if constexpr (std::is_pointer_v<Bare> &&
                           std::is_base_of_v<Object, std::remove_cv_t<std::remove_pointer_t<Bare>>>)
        {
            using U = std::remove_cv_t<std::remove_pointer_t<Bare>>;
            return v.AsObject<U>();
        }
        else if constexpr (std::is_class_v<Bare> && std::is_base_of_v<Object, Bare>)
        {
            return *v.AsObject<Bare>();
        }
        else if constexpr (ArgArray<Bare>::value)
        {
            // BY VALUE (a fresh Array<E> from a script's list, or a copy of a native one), so
            // decltype(auto) stays consistent across this branch. AcceptArg checked every element.
            if (const Bare* typed = v.template TryGet<Bare>())
            {
                return Bare(*typed);
            }
            Bare out;
            if (Array<Variant>* list = v.template TryGet<Array<Variant>>())
            {
                out.Reserve(list->Size());
                for (Variant& element : *list)
                {
                    out.PushBack(ConvertArg<typename ArgArray<Bare>::Element>(element));
                }
            }
            return out;
        }
        else if constexpr (std::is_enum_v<Bare>)
        {
            // An enum argument crosses from script as its underlying INT (no native enum type in
            // script backends; AngelScript enums are int-backed), so accept an i64/f64 and cast - as
            // well as a properly-typed enum Variant. Bare is known here, so the cast is well-
            // defined. Mirrors the property-setter coercion (PropertySet). Returns BY VALUE, so
            // decltype(auto) stays consistent across this branch.
            if (const i64* i = v.template TryGet<i64>())
            {
                return static_cast<Bare>(*i);
            }
            if (const f64* d = v.template TryGet<f64>())
            {
                return static_cast<Bare>(static_cast<i64>(*d));
            }
            if (const Bare* typed = v.template TryGet<Bare>())
            {
                return static_cast<Bare>(*typed);
            }
            return Bare{};
        }
        else
        {
            return *v.template TryGet<Bare>();
        }
    }

    template <typename... A>
    [[nodiscard]] Span<const ParamInfo> MakeParams()
    {
        if constexpr (sizeof...(A) == 0)
        {
            return Span<const ParamInfo>{};
        }
        else
        {
            static const ParamInfo params[] = {ParamInfo{&ParamTypeOf<A>, ""}...};
            return Span<const ParamInfo>{params, sizeof...(A)};
        }
    }

    template <typename... A, usize... I>
    [[nodiscard]] bool ArgsMatch(Span<Variant>& args, std::index_sequence<I...>)
    {
        return (... && AcceptArg<A>(args[I]));
    }

    template <auto Member, typename C, typename R, bool Const, typename... A, usize... I>
    Result<Variant> InvokeMemberImpl(const Instance& instance, Span<Variant> args,
                                     std::index_sequence<I...> seq)
    {
        if (args.Size() != sizeof...(A))
        {
            return Err(ErrorCode::InvalidArgument);
        }
        if constexpr (sizeof...(A) > 0)
        {
            if (!ArgsMatch<A...>(args, seq))
            {
                return Err(ErrorCode::InvalidArgument);
            }
        }
        using ObjectType = std::conditional_t<Const, const C, C>;
        ObjectType* object = static_cast<ObjectType*>(instance.Pointer());
        if constexpr (std::is_void_v<R>)
        {
            (object->*Member)(ConvertArg<A>(args[I])...);
            return Variant{};
        }
        else
        {
            return Variant::From<std::remove_cvref_t<R>>(
                (object->*Member)(ConvertArg<A>(args[I])...));
        }
    }

    template <auto Func, typename R, typename... A, usize... I>
    Result<Variant> InvokeFreeImpl(Span<Variant> args, std::index_sequence<I...> seq)
    {
        if (args.Size() != sizeof...(A))
        {
            return Err(ErrorCode::InvalidArgument);
        }
        if constexpr (sizeof...(A) > 0)
        {
            if (!ArgsMatch<A...>(args, seq))
            {
                return Err(ErrorCode::InvalidArgument);
            }
        }
        if constexpr (std::is_void_v<R>)
        {
            Func(ConvertArg<A>(args[I])...);
            return Variant{};
        }
        else
        {
            return Variant::From<std::remove_cvref_t<R>>(Func(ConvertArg<A>(args[I])...));
        }
    }

    template <typename R>
    [[nodiscard]] const TypeInfo* ReturnTypeInfo() noexcept
    {
        if constexpr (std::is_void_v<R>)
        {
            return nullptr;
        }
        else
        {
            return ParamTypeOf<R>();
        } // object-aware (StaticType for objects)
    }

    template <typename T, typename... A, usize... I>
    Result<Variant> ConstructImpl(Span<Variant> args, std::index_sequence<I...> seq)
    {
        if (args.Size() != sizeof...(A))
        {
            return Err(ErrorCode::InvalidArgument);
        }
        if constexpr (sizeof...(A) > 0)
        {
            if (!ArgsMatch<A...>(args, seq))
            {
                return Err(ErrorCode::InvalidArgument);
            }
        }
        if constexpr (std::is_base_of_v<Object, T>)
        {
            // Object-derived: heap-allocate via MakeRef -> Variant object mode.
            return Variant::From(MakeRef<T>(DefaultAllocator(), ConvertArg<A>(args[I])...));
        }
        else
        {
            return Variant::From<T>(T(ConvertArg<A>(args[I])...));
        }
    }

    template <typename T, typename... A>
    struct ConstructorReflect
    {
        static constexpr usize kArity = sizeof...(A);
        static Span<const ParamInfo> Params() { return MakeParams<A...>(); }
        static Result<Variant> Invoke(Span<Variant> args)
        {
            return ConstructImpl<T, A...>(args, std::index_sequence_for<A...>{});
        }
    };

    template <auto Member, typename Sig = decltype(Member)>
    struct MethodReflect;

    template <auto Member, typename C, typename R, typename... A> // instance method
    struct MethodReflect<Member, R (C::*)(A...)>
    {
        static constexpr bool isStatic = false;
        static constexpr bool isConst = false;
        static const TypeInfo* ReturnType() { return ReturnTypeInfo<R>(); }
        static constexpr usize kArity = sizeof...(A);
        static Span<const ParamInfo> Params() { return MakeParams<A...>(); }
        static Result<Variant> Invoke(const Instance& i, Span<Variant> a)
        {
            return InvokeMemberImpl<Member, C, R, false, A...>(i, a,
                                                               std::index_sequence_for<A...>{});
        }
    };

    template <auto Member, typename C, typename R, typename... A> // const instance method
    struct MethodReflect<Member, R (C::*)(A...) const>
    {
        static constexpr bool isStatic = false;
        static constexpr bool isConst = true;
        static const TypeInfo* ReturnType() { return ReturnTypeInfo<R>(); }
        static constexpr usize kArity = sizeof...(A);
        static Span<const ParamInfo> Params() { return MakeParams<A...>(); }
        static Result<Variant> Invoke(const Instance& i, Span<Variant> a)
        {
            return InvokeMemberImpl<Member, C, R, true, A...>(i, a,
                                                              std::index_sequence_for<A...>{});
        }
    };

    template <auto Func, typename R, typename... A> // static / free function
    struct MethodReflect<Func, R (*)(A...)>
    {
        static constexpr bool isStatic = true;
        static constexpr bool isConst = false;
        static const TypeInfo* ReturnType() { return ReturnTypeInfo<R>(); }
        static constexpr usize kArity = sizeof...(A);
        static Span<const ParamInfo> Params() { return MakeParams<A...>(); }
        static Result<Variant> Invoke(const Instance&, Span<Variant> a)
        {
            return InvokeFreeImpl<Func, R, A...>(a, std::index_sequence_for<A...>{});
        }
    };

    // noexcept is part of the function type (C++17), so each form needs a
    // noexcept twin. These delegate to the same invoke implementations.
    template <auto Member, typename C, typename R, typename... A> // instance method (noexcept)
    struct MethodReflect<Member, R (C::*)(A...) noexcept>
    {
        static constexpr bool isStatic = false;
        static constexpr bool isConst = false;
        static const TypeInfo* ReturnType() { return ReturnTypeInfo<R>(); }
        static constexpr usize kArity = sizeof...(A);
        static Span<const ParamInfo> Params() { return MakeParams<A...>(); }
        static Result<Variant> Invoke(const Instance& i, Span<Variant> a)
        {
            return InvokeMemberImpl<Member, C, R, false, A...>(i, a,
                                                               std::index_sequence_for<A...>{});
        }
    };

    template <auto Member, typename C, typename R,
              typename... A> // const instance method (noexcept)
    struct MethodReflect<Member, R (C::*)(A...) const noexcept>
    {
        static constexpr bool isStatic = false;
        static constexpr bool isConst = true;
        static const TypeInfo* ReturnType() { return ReturnTypeInfo<R>(); }
        static constexpr usize kArity = sizeof...(A);
        static Span<const ParamInfo> Params() { return MakeParams<A...>(); }
        static Result<Variant> Invoke(const Instance& i, Span<Variant> a)
        {
            return InvokeMemberImpl<Member, C, R, true, A...>(i, a,
                                                              std::index_sequence_for<A...>{});
        }
    };

    template <auto Func, typename R, typename... A> // static / free function (noexcept)
    struct MethodReflect<Func, R (*)(A...) noexcept>
    {
        static constexpr bool isStatic = true;
        static constexpr bool isConst = false;
        static const TypeInfo* ReturnType() { return ReturnTypeInfo<R>(); }
        static constexpr usize kArity = sizeof...(A);
        static Span<const ParamInfo> Params() { return MakeParams<A...>(); }
        static Result<Variant> Invoke(const Instance&, Span<Variant> a)
        {
            return InvokeFreeImpl<Func, R, A...>(a, std::index_sequence_for<A...>{});
        }
    };

    // Dispatch for a method reflected with an OVERRIDDEN declared return type (Method<Member,
    // ReturnAs>): run the normal invoke, then VALIDATE the returned Variant's runtime type matches
    // `ReturnAs`. A mismatch resolves to empty - never surface a type-confused handle to a backend
    // that trusts the declared return type. For factories returning a RESOLVE-mode handle of a
    // specific type (RigidBody.of(entity)); composes with Variant::From<Variant> passthrough.
    template <auto Member, typename ReturnAs>
    [[nodiscard]] Result<Variant> InvokeReturningAs(const Instance& instance, Span<Variant> args)
    {
        Result<Variant> result = MethodReflect<Member>::Invoke(instance, args);
        // Validate against the SAME TypeInfo the declared return uses (ReturnTypeInfo is
        // object-aware: StaticType for objects), so a legitimate return is not spuriously rejected.
        if (result.HasValue() && result.Value().Type() != ReturnTypeInfo<ReturnAs>())
        {
            return Variant{};
        }
        return result;
    }
}

export namespace foundation::core
{
    // Holds a type's TypeInfo together with the property/method arrays it points
    // into. Stored as a single static (see REFLECT_MEMBERS); Array's move
    // preserves the buffer address, so the TypeInfo pointers stay valid.
    struct TypeData
    {
        Array<PropertyInfo> properties;
        Array<Array<Attribute>> propertyAttributes; // parallel to `properties`
        Array<MethodInfo> methods;
        Array<Attribute> attributes;
        Array<ConstantInfo> constants;
        Array<ConstructorInfo> constructors;
        // Stable storage for method/constructor parameter names authored via the named
        // Method()/Constructor() overloads (C++ cannot recover them). Each entry backs one
        // method's/ctor's ParamInfo array; inner buffers survive TypeData's move and outer
        // growth (Array move steals the buffer), so the ParamInfo* stored in MethodInfo stays valid.
        Array<Array<ParamInfo>> namedParams;
        TypeInfo info{};
    };

    template <typename T>
    class TypeBuilder
    {
    public:
        TypeBuilder(const char* name, const char* namespaceName, const TypeInfo* base) noexcept
            : m_name(name), m_namespace(namespaceName), m_base(base)
        {
        }

        /// Serialization data version (migration): bump when the serialized layout changes.
        TypeBuilder& DataVersion(u32 version)
        {
            m_dataVersion = version;
            return *this;
        }

        /// Keep a legacy reader: a payload stored under any version from `oldest` up to the
        /// current one is accepted, and the Serialize body branches on ar.Version() for the old
        /// layouts. One version back, removed once the data has moved.
        TypeBuilder& ReadsDataVersionsFrom(u32 oldest)
        {
            m_minReadDataVersion = oldest;
            return *this;
        }

        template <auto Member>
        TypeBuilder& Property(const char* name, PropertyFlags flags = PropertyFlags::None)
        {
            using M = typename detail::MemberTraits<decltype(Member)>::Member;
            m_data.properties.PushBack(PropertyInfo{
                name, &TypeOf<M>(), flags, &detail::PropertyGet<T, M, Member>,
                &detail::PropertySet<T, M, Member>, &detail::PropertyAddress<T, M, Member>});
            return *this;
        }

        /// A COMPUTED read-only property backed by a const zero-arg getter (returns by value).
        /// Emits as a parens-less getter in the script backends and reflects as a property to
        /// tooling, but the value is derived rather than a stored field: `address` is null (no
        /// in-place editing) and set returns NotSupported. Use for facade accessors that should
        /// read as a property (`entity.scene`) rather than a method (`entity.scene()`).
        template <auto Getter>
        TypeBuilder& ComputedProperty(const char* name)
        {
            using R = typename detail::GetterTraits<decltype(Getter)>::Return;
            m_data.properties.PushBack(PropertyInfo{name, &TypeOf<R>(), PropertyFlags::ReadOnly,
                                                    &detail::GetterPropertyGet<T, R, Getter>,
                                                    &detail::GetterPropertySet<T, R, Getter>,
                                                    nullptr});
            return *this;
        }

        /// A WRITABLE computed property: a const zero-arg getter and a one-argument setter over
        /// state the type keeps behind an invariant (a flag inside a nested value whose caches
        /// must rebuild when it changes). Reflects as an ordinary editable property to tooling
        /// and script, with `address` null (no in-place editing: every write goes through the
        /// setter). The getter's return and the setter's argument must be the same type.
        template <auto Getter, auto Setter>
        TypeBuilder& AccessorProperty(const char* name)
        {
            using R = typename detail::GetterTraits<decltype(Getter)>::Return;
            static_assert(std::is_same_v<R, typename detail::SetterTraits<decltype(Setter)>::Argument>,
                          "AccessorProperty: the getter's return and the setter's argument differ");
            m_data.properties.PushBack(PropertyInfo{name, &TypeOf<R>(), PropertyFlags::None,
                                                    &detail::GetterPropertyGet<T, R, Getter>,
                                                    &detail::SetterPropertySet<T, R, Setter>,
                                                    nullptr});
            return *this;
        }

        /// A NESTED structure property: `member` is itself a reflected type the tooling recurses
        /// into (a `MaterialSource source;` value member, or a `MeshSource* source;` pointer
        /// member). Unlike Property<>, it never copies the member through a Variant, so it works
        /// for non-copyable Object members. `get` is empty, `set` returns NotSupported, `type` is
        /// the nested type, and `address` yields the member (the POINTEE for a pointer member, null
        /// when unset). Consumers check IsNested(), then recurse via address; script harvest skips it.
        template <auto Member>
        TypeBuilder& Nested(const char* name)
        {
            using M = typename detail::MemberTraits<decltype(Member)>::Member;
            using Pointee = typename detail::NestedPointee<M>::Type;
            m_data.properties.PushBack(PropertyInfo{name, detail::NestedTypeInfo<Pointee>(),
                                                    PropertyFlags::Nested,
                                                    &detail::NestedPropertyGet,
                                                    &detail::NestedPropertySet,
                                                    &detail::NestedPropertyAddress<T, M, Member>});
            return *this;
        }

        /// A BOUNDED-ARRAY property: a fixed-capacity C-array member (`T member[N]`) paired with a
        /// live count member (the logical length). Reflects as a container whose size is the count
        /// clamped to [0, N] - iterating never surfaces garbage beyond the count - reusing the whole
        /// IsContainer/ContainerGetAt/ContainerSetAt consumer path. Flagged Nested (address = the
        /// owner; the container reads the array + count off it); harvest skips it like any Nested.
        template <auto ArrayMember, auto CountMember>
        TypeBuilder& BoundedArray(const char* name)
        {
            using AT = detail::CArrayTraits<decltype(ArrayMember)>;
            using Owner = typename AT::Owner;
            using Elem = typename AT::Element;
            constexpr usize N = AT::Extent;
            static const ContainerInfo container{
                .elementType = &TypeOf<Elem>(),
                .size = &detail::BoundedArraySize<Owner, Elem, N, ArrayMember, CountMember>,
                .getAt = &detail::BoundedArrayGetAt<Owner, Elem, N, ArrayMember, CountMember>,
                .setAt = &detail::BoundedArraySetAt<Owner, Elem, N, ArrayMember, CountMember>,
                .flags = ContainerFlags::None,
                .createElement = nullptr,
                .canCreateElement = nullptr,
                .emplaceDefault =
                    &detail::BoundedEmplaceDefault<Owner, Elem, N, ArrayMember, CountMember>,
                .removeAt = &detail::BoundedRemoveAt<Owner, Elem, N, ArrayMember, CountMember>,
                .moveElement = &detail::BoundedMove<Owner, Elem, N, ArrayMember, CountMember>};
            static const TypeInfo boundedType = []()
            {
                TypeInfo info = MakeTypeInfo<Elem>("BoundedArray", "rtti::core", nullptr);
                info.container = &container;
                return info;
            }();
            m_data.properties.PushBack(PropertyInfo{name, &boundedType, PropertyFlags::Nested,
                                                    &detail::NestedPropertyGet,
                                                    &detail::NestedPropertySet,
                                                    &detail::BoundedArrayAddress});
            return *this;
        }

        template <auto Member>
        TypeBuilder& Method(const char* name)
        {
            using Reflect = detail::MethodReflect<Member>;
            const Span<const ParamInfo> params = Reflect::Params();
            m_data.methods.PushBack(MethodInfo{name, &Reflect::ReturnType, params.Data(),
                                               static_cast<u32>(params.Size()), Reflect::isStatic,
                                               Reflect::isConst, &Reflect::Invoke});
            return *this;
        }

        /// Give the LAST-added method a distinct alternate name for consumers that bind one entry
        /// per name (scripting). Call fluently after Method():
        /// `.Method<&T::SetPositionXyz>("setPosition").OverloadedName("setPositionXyz")`.
        /// The contract is DISTINCTNESS, not annotate-everything: an N-member overload set needs
        /// only enough names that no two collide, so the primary overload keeps the clean name
        /// (`setPosition` + `setPositionXyz`). A non-overloaded method never needs it.
        TypeBuilder& OverloadedName(const char* scriptName)
        {
            // A misplaced fluent call (before any Method()) is an authoring bug, not a silent no-op.
            DIAGNOSTIC_ASSERT(m_data.methods.Size() > 0 &&
                              "OverloadedName() must follow a Method()");
            m_data.methods[m_data.methods.Size() - 1].overloadedName = scriptName;
            return *this;
        }

        /// Mark the LAST-added method as implementing `op` (MethodOperator): a static whose first
        /// parameter is the left operand. Call fluently after Method():
        /// `.Method<static_cast<Float3 (*)(Float3, Float3)>(&operator+)>("Add").Operator(Add)`.
        TypeBuilder& Operator(MethodOperator op)
        {
            DIAGNOSTIC_ASSERT(m_data.methods.Size() > 0 && "Operator() must follow a Method()");
            DIAGNOSTIC_ASSERT(m_data.methods[m_data.methods.Size() - 1].isStatic &&
                              "an operator is a static whose first parameter is the left operand");
            m_data.methods[m_data.methods.Size() - 1].op = op;
            return *this;
        }

        /// Reflect a method WITH explicit parameter names. C++ cannot recover parameter names, so
        /// pass them here to give bindings/tooling real names instead of arg0..N. The list length
        /// is checked against the method's arity AT COMPILE TIME - name all parameters or none.
        template <auto Member, usize N>
        TypeBuilder& Method(const char* name, const char* const (&paramNames)[N])
        {
            using Reflect = detail::MethodReflect<Member>;
            static_assert(N == Reflect::kArity,
                          "reflected parameter-name list length must equal the method's arity");
            const ParamInfo* params = StoreNamedParams(Reflect::Params(), paramNames);
            m_data.methods.PushBack(MethodInfo{name, &Reflect::ReturnType, params,
                                               static_cast<u32>(N), Reflect::isStatic,
                                               Reflect::isConst, &Reflect::Invoke});
            return *this;
        }

        /// Reflect a method whose C++ body returns a Variant but whose DECLARED reflected return type
        /// is `ReturnAs` - a factory that hands back a runtime-typed handle (e.g. `RigidBody.of(entity)`
        /// returning a RESOLVE-mode ref whose dynamic type is the component). The declared return
        /// drives AngelScript's static boxing. The dispatch
        /// validates the returned Variant's runtime type equals `ReturnAs` (mismatch -> empty, never a
        /// type-confused handle). Composes with Variant::From<Variant> passthrough.
        template <auto Member, typename ReturnAs>
        TypeBuilder& Method(const char* name)
        {
            using Reflect = detail::MethodReflect<Member>;
            const Span<const ParamInfo> params = Reflect::Params();
            m_data.methods.PushBack(MethodInfo{name, &detail::ReturnTypeInfo<ReturnAs>, params.Data(),
                                               static_cast<u32>(params.Size()), Reflect::isStatic,
                                               Reflect::isConst,
                                               &detail::InvokeReturningAs<Member, ReturnAs>});
            return *this;
        }

        template <typename V>
        TypeBuilder& Attribute(const char* key, V value)
        {
            m_data.attributes.PushBack(
                foundation::core::Attribute{key, Variant::From<V>(Move(value))});
            return *this;
        }

        /// Attach an attribute to the MOST RECENTLY added property (fluent - call right
        /// after Property()). Tooling metadata: "range" (Float4 min/max/step), "visibleWhen"
        /// (String "prop" truthy or "prop=1,2" value list), etc.
        template <typename V>
        TypeBuilder& PropAttribute(const char* key, V value)
        {
            if (m_data.properties.IsEmpty())
            {
                return *this;
            }
            while (m_data.propertyAttributes.Size() < m_data.properties.Size())
            {
                m_data.propertyAttributes.PushBack(Array<foundation::core::Attribute>{});
            }
            m_data.propertyAttributes[m_data.properties.Size() - 1].PushBack(
                foundation::core::Attribute{key, Variant::From<V>(Move(value))});
            return *this;
        }

        template <typename V>
        TypeBuilder& Constant(const char* name, V value)
        {
            m_data.constants.PushBack(
                foundation::core::ConstantInfo{name, &TypeOf<V>(), Variant::From<V>(Move(value))});
            return *this;
        }

        // Reflects a constructor T(Args...). Call multiple times for overloads.
        template <typename... Args>
        TypeBuilder& Constructor()
        {
            using Reflect = detail::ConstructorReflect<T, Args...>;
            const Span<const ParamInfo> params = Reflect::Params();
            m_data.constructors.PushBack(
                ConstructorInfo{params.Data(), static_cast<u32>(params.Size()), &Reflect::Invoke});
            return *this;
        }

        // Reflects a constructor T(Args...) WITH explicit parameter names (arity-checked at
        // compile time; name all parameters or none).
        template <typename... Args, usize N>
        TypeBuilder& Constructor(const char* const (&paramNames)[N])
        {
            using Reflect = detail::ConstructorReflect<T, Args...>;
            static_assert(N == Reflect::kArity,
                          "reflected parameter-name list length must equal the constructor's arity");
            const ParamInfo* params = StoreNamedParams(Reflect::Params(), paramNames);
            m_data.constructors.PushBack(
                ConstructorInfo{params, static_cast<u32>(N), &Reflect::Invoke});
            return *this;
        }

        [[nodiscard]] TypeData Build()
        {
            m_data.info = MakeTypeInfo<T>(m_name, m_namespace, m_base, m_dataVersion);
            m_data.info.minReadDataVersion = m_minReadDataVersion;
            // Wire per-property attribute spans (the inner Array buffers survive TypeData's
            // move - only the outer array's control block moves).
            while (m_data.propertyAttributes.Size() < m_data.properties.Size())
            {
                m_data.propertyAttributes.PushBack(Array<foundation::core::Attribute>{});
            }
            for (usize i = 0; i < m_data.properties.Size(); ++i)
            {
                m_data.properties[i].attributes = m_data.propertyAttributes[i].Data();
                m_data.properties[i].attributeCount =
                    static_cast<u32>(m_data.propertyAttributes[i].Size());
            }
            m_data.info.properties = m_data.properties.Data();
            m_data.info.propertyCount = static_cast<u32>(m_data.properties.Size());
            m_data.info.methods = m_data.methods.Data();
            m_data.info.methodCount = static_cast<u32>(m_data.methods.Size());
            m_data.info.attributes = m_data.attributes.Data();
            m_data.info.attributeCount = static_cast<u32>(m_data.attributes.Size());
            m_data.info.constants = m_data.constants.Data();
            m_data.info.constantCount = static_cast<u32>(m_data.constants.Size());
            m_data.info.constructors = m_data.constructors.Data();
            m_data.info.constructorCount = static_cast<u32>(m_data.constructors.Size());
            return Move(m_data);
        }

    private:
        // Copy `base`'s type-getters but substitute the authored names, into stable TypeData
        // storage. The returned pointer stays valid across TypeData's move + later namedParams
        // growth (Array move steals the inner buffer). Called only for N >= 1 (named overloads).
        template <usize N>
        const ParamInfo* StoreNamedParams(Span<const ParamInfo> base,
                                          const char* const (&names)[N])
        {
            Array<ParamInfo> named;
            named.Reserve(N);
            for (usize i = 0; i < N; ++i)
            {
                named.PushBack(ParamInfo{base[i].type, names[i]});
            }
            m_data.namedParams.PushBack(Move(named));
            return m_data.namedParams.Back().Data();
        }

        const char* m_name;
        const char* m_namespace;
        const TypeInfo* m_base;
        u32 m_dataVersion = 0;
        u32 m_minReadDataVersion = 0;
        TypeData m_data;
    };
}
