// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - RTTI declaration macros (classic header).
//
// Use inside an Object-derived class to wire up type identity, then define the
// type once in a .cpp. Registration stays explicit: call
// GlobalTypeRegistry().Register(Type::StaticType()) from a RegisterTypes()
// function.
//
//   // header
//   class Entity : public Object { RTTI_OBJECT(Entity, Object) public: ... };
//   // source
//   RTTI_DEFINE_OBJECT(Entity, "rtti::game")

#ifndef FOUNDATION_CORE_RTTI_REFLECT_H
#define FOUNDATION_CORE_RTTI_REFLECT_H

#include "Core/Prelude.h"

// Declares static/virtual type accessors. Leaves access as `public:`.
#define RTTI_OBJECT(Type, BaseType)                                                            \
public:                                                                                            \
    using Super = BaseType;                                                                        \
    static const ::foundation::core::TypeInfo& StaticType() noexcept;                                \
    const ::foundation::core::TypeInfo* GetType() const noexcept override { return &StaticType(); }

// Defines StaticType() for a Type with no reflected properties.
#define RTTI_DEFINE_OBJECT(Type, Namespace)                                                    \
    const ::foundation::core::TypeInfo& Type::StaticType() noexcept                                  \
    {                                                                                              \
        static const ::foundation::core::TypeInfo info =                                             \
            ::foundation::core::MakeTypeInfo<Type>(#Type, Namespace, &Super::StaticType());          \
        return info;                                                                               \
    }

// RTTI_DEFINE_OBJECT with a serialization data version (migration): bump the number when
// the type's serialized layout changes; the Serialize body branches on ar.Version().
#define RTTI_DEFINE_OBJECT_VERSIONED(Type, Namespace, DataVersion)                             \
    const ::foundation::core::TypeInfo& Type::StaticType() noexcept                                  \
    {                                                                                              \
        static const ::foundation::core::TypeInfo info = ::foundation::core::MakeTypeInfo<Type>(       \
            #Type, Namespace, &Super::StaticType(), DataVersion);                                  \
        return info;                                                                               \
    }

// Defines StaticType() with a reflection body that configures `builder`, e.g.:
//   REFLECT_MEMBERS(Entity, "rtti::game")
//   {
//       builder.Property<&Entity::name>("name");
//   }
#define REFLECT_MEMBERS(Type, Namespace)                                                          \
    static void RttiReflect_##Type(::foundation::core::TypeBuilder<Type>& builder);              \
    const ::foundation::core::TypeInfo& Type::StaticType() noexcept                                  \
    {                                                                                              \
        static ::foundation::core::TypeData rttiTypeData = []()                                  \
        {                                                                                          \
            ::foundation::core::TypeBuilder<Type> builder(#Type, Namespace, &Super::StaticType());   \
            RttiReflect_##Type(builder);                                                       \
            return builder.Build();                                                                \
        }();                                                                                       \
        return rttiTypeData.info;                                                              \
    }                                                                                              \
    static void RttiReflect_##Type(                                                            \
        [[maybe_unused]] ::foundation::core::TypeBuilder<Type>& builder)

// Reflects an enum's named values. Defines a registration function
// RttiRegisterEnum_<EnumType>() to be called explicitly at startup, e.g.:
//   REFLECT_ENUM(Color, "rtti::game")
//   {
//       builder.Value("Red", Color::Red);
//       builder.Value("Green", Color::Green);
//   }
//   // then: RttiRegisterEnum_Color();
#define REFLECT_ENUM(EnumType, Namespace)                                                 \
    static void RttiEnumBody_##EnumType(::foundation::core::EnumBuilder<EnumType>&);             \
    void RttiRegisterEnum_##EnumType()                                                         \
    {                                                                                              \
        /* Once and thread-safe: builders re-register from cook workers, and a rebuild moves */   \
        /* a fresh enumerator array over the live one another thread may be reading.        */   \
        static const bool rttiOnce = []()                                                          \
        {                                                                                          \
            ::foundation::core::EnumBuilder<EnumType> builder(#EnumType, Namespace);                 \
            RttiEnumBody_##EnumType(builder);                                                  \
            builder.Build();                                                                       \
            return true;                                                                           \
        }();                                                                                       \
        (void)rttiOnce;                                                                            \
    }                                                                                              \
    static void RttiEnumBody_##EnumType(                                                       \
        [[maybe_unused]] ::foundation::core::EnumBuilder<EnumType>& builder)

// Reflects a non-Object value type (plain struct) non-intrusively: builds its
// properties/methods with a TypeBuilder and patches the type's TypeOf<T>() in
// place (so it gains a qualified name/id + members without an intrusive
// StaticType()). Defines RttiRegisterValue_<Type>() to call once at startup,
// e.g.:
//   REFLECT_VALUE(Float3, "rtti::core")
//   {
//       builder.Property<&Float3::x>("x").Property<&Float3::y>("y").Property<&Float3::z>("z");
//   }
//   // then: RttiRegisterValue_Float3();
#define REFLECT_VALUE(Type, Namespace)                                                    \
    static void RttiReflectValue_##Type(::foundation::core::TypeBuilder<Type>& builder);         \
    void RttiRegisterValue_##Type()                                                            \
    {                                                                                              \
        /* The in-place patch of TypeOf<Type>() happens once too (thread-safe static init): */    \
        /* re-copying identical bytes over a TypeInfo other threads read is still a race.   */    \
        static const bool rttiOnce = []()                                                          \
        {                                                                                          \
            static ::foundation::core::TypeData rttiTypeData = []()                              \
            {                                                                                      \
                ::foundation::core::TypeBuilder<Type> builder(#Type, Namespace, nullptr);            \
                RttiReflectValue_##Type(builder);                                              \
                return builder.Build();                                                            \
            }();                                                                                   \
            const_cast<::foundation::core::TypeInfo&>(::foundation::core::TypeOf<Type>()) =            \
                rttiTypeData.info;                                                             \
            return true;                                                                           \
        }();                                                                                       \
        (void)rttiOnce;                                                                            \
    }                                                                                              \
    static void RttiReflectValue_##Type(                                                       \
        [[maybe_unused]] ::foundation::core::TypeBuilder<Type>& builder)

#endif // FOUNDATION_CORE_RTTI_REFLECT_H
