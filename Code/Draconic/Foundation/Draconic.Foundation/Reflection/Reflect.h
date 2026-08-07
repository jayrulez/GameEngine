// Draconic Foundation - RTTI declaration macros (classic header).
//
// Use inside an Object-derived class to wire up type identity, then define the
// type once in a .cpp. Registration stays explicit - call
// GlobalTypeRegistry().Register(Type::StaticType()) from a RegisterTypes()
// function (see Documentation/Planning/Core.md §4.10).
//
//   // header
//   class Entity : public Object { DRACONIC_OBJECT(Entity, Object) public: ... };
//   // source
//   DRACONIC_DEFINE_OBJECT(Entity, "draconic::game")

#ifndef DRACONIC_FOUNDATION_RTTI_REFLECT_H
#define DRACONIC_FOUNDATION_RTTI_REFLECT_H

#include "Draconic.Foundation/Prelude.h"

// Declares static/virtual type accessors. Leaves access as `public:`.
#define DRACONIC_OBJECT(Type, BaseType)                                                            \
public:                                                                                            \
    using Super = BaseType;                                                                        \
    static const ::draconic::foundation::TypeInfo& StaticType() noexcept;                                \
    const ::draconic::foundation::TypeInfo* GetType() const noexcept override { return &StaticType(); }

// Defines StaticType() for a Type with no reflected properties.
#define DRACONIC_DEFINE_OBJECT(Type, Namespace)                                                    \
    const ::draconic::foundation::TypeInfo& Type::StaticType() noexcept                                  \
    {                                                                                              \
        static const ::draconic::foundation::TypeInfo info =                                             \
            ::draconic::foundation::MakeTypeInfo<Type>(#Type, Namespace, &Super::StaticType());          \
        return info;                                                                               \
    }

// DRACONIC_DEFINE_OBJECT with a serialization DATA VERSION (migration): bump the number when
// the type's serialized layout changes; the Serialize body branches on ar.Version().
#define DRACONIC_DEFINE_OBJECT_VERSIONED(Type, Namespace, DataVersion)                             \
    const ::draconic::foundation::TypeInfo& Type::StaticType() noexcept                                  \
    {                                                                                              \
        static const ::draconic::foundation::TypeInfo info = ::draconic::foundation::MakeTypeInfo<Type>(       \
            #Type, Namespace, &Super::StaticType(), DataVersion);                                  \
        return info;                                                                               \
    }

// Defines StaticType() with a reflection body that configures `builder`, e.g.:
//   DRACONIC_REFLECT(Entity, "draconic::game")
//   {
//       builder.Property<&Entity::name>("name");
//   }
#define DRACONIC_REFLECT(Type, Namespace)                                                          \
    static void DraconicReflect_##Type(::draconic::foundation::TypeBuilder<Type>& builder);              \
    const ::draconic::foundation::TypeInfo& Type::StaticType() noexcept                                  \
    {                                                                                              \
        static ::draconic::foundation::TypeData draconicTypeData = []()                                  \
        {                                                                                          \
            ::draconic::foundation::TypeBuilder<Type> builder(#Type, Namespace, &Super::StaticType());   \
            DraconicReflect_##Type(builder);                                                       \
            return builder.Build();                                                                \
        }();                                                                                       \
        return draconicTypeData.info;                                                              \
    }                                                                                              \
    static void DraconicReflect_##Type(                                                            \
        [[maybe_unused]] ::draconic::foundation::TypeBuilder<Type>& builder)

// Reflects an enum's named values. Defines a registration function
// DraconicRegisterEnum_<EnumType>() to be called explicitly at startup, e.g.:
//   DRACONIC_REFLECT_ENUM(Color, "draconic::game")
//   {
//       builder.Value("Red", Color::Red);
//       builder.Value("Green", Color::Green);
//   }
//   // later: DraconicRegisterEnum_Color();
#define DRACONIC_REFLECT_ENUM(EnumType, Namespace)                                                 \
    static void DraconicEnumBody_##EnumType(::draconic::foundation::EnumBuilder<EnumType>&);             \
    void DraconicRegisterEnum_##EnumType()                                                         \
    {                                                                                              \
        ::draconic::foundation::EnumBuilder<EnumType> builder(#EnumType, Namespace);                     \
        DraconicEnumBody_##EnumType(builder);                                                      \
        builder.Build();                                                                           \
    }                                                                                              \
    static void DraconicEnumBody_##EnumType(                                                       \
        [[maybe_unused]] ::draconic::foundation::EnumBuilder<EnumType>& builder)

// Reflects a non-Object value type (plain struct) non-intrusively: builds its
// properties/methods with a TypeBuilder and patches the type's TypeOf<T>() in
// place (so it gains a qualified name/id + members without an intrusive
// StaticType()). Defines DraconicRegisterValue_<Type>() to call once at startup,
// e.g.:
//   DRACONIC_REFLECT_VALUE(Float3, "draconic::foundation")
//   {
//       builder.Property<&Float3::x>("x").Property<&Float3::y>("y").Property<&Float3::z>("z");
//   }
//   // later: DraconicRegisterValue_Float3();
#define DRACONIC_REFLECT_VALUE(Type, Namespace)                                                    \
    static void DraconicReflectValue_##Type(::draconic::foundation::TypeBuilder<Type>& builder);         \
    void DraconicRegisterValue_##Type()                                                            \
    {                                                                                              \
        static ::draconic::foundation::TypeData draconicTypeData = []()                                  \
        {                                                                                          \
            ::draconic::foundation::TypeBuilder<Type> builder(#Type, Namespace, nullptr);                \
            DraconicReflectValue_##Type(builder);                                                  \
            return builder.Build();                                                                \
        }();                                                                                       \
        const_cast<::draconic::foundation::TypeInfo&>(::draconic::foundation::TypeOf<Type>()) =                \
            draconicTypeData.info;                                                                 \
    }                                                                                              \
    static void DraconicReflectValue_##Type(                                                       \
        [[maybe_unused]] ::draconic::foundation::TypeBuilder<Type>& builder)

#endif // DRACONIC_FOUNDATION_RTTI_REFLECT_H
