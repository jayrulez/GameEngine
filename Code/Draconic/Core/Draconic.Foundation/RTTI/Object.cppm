// Draconic Foundation - :object partition
//
// Object: the polymorphic reflection root (derives RefCounted), plus the
// Cast/IsA helpers that replace dynamic_cast by walking the base chain.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:object;

import :base;
import :ref_counted;
import :type_info;

export namespace draconic::foundation
{
    // =======================================================================
    // Object - polymorphic reflection root. Derives from RefCounted, so every
    // Object is held via RefPtr<Object> (§4.10).
    // =======================================================================
    class Object : public RefCounted
    {
    public:
        using Super = void;

        [[nodiscard]] virtual const TypeInfo* GetType() const noexcept { return &StaticType(); }

        [[nodiscard]] static const TypeInfo& StaticType() noexcept
        {
            static const TypeInfo info{ComputeTypeId("draconic::foundation", "Object"),
                                       "Object",
                                       "draconic::foundation",
                                       static_cast<u32>(sizeof(Object)),
                                       static_cast<u32>(alignof(Object)),
                                       nullptr};
            return info;
        }
    };

    // =======================================================================
    // Cast / IsA - replace dynamic_cast by walking the single-inheritance chain.
    // =======================================================================
    [[nodiscard]] inline bool IsDerivedFrom(const TypeInfo* type, const TypeInfo* base) noexcept
    {
        for (const TypeInfo* t = type; t != nullptr; t = t->base)
        {
            if (t == base)
            {
                return true;
            }
        }
        return false;
    }

    template <typename T>
    [[nodiscard]] bool IsA(const Object* object) noexcept
    {
        return object != nullptr && IsDerivedFrom(object->GetType(), &T::StaticType());
    }

    template <typename T>
    [[nodiscard]] T* Cast(Object* object) noexcept
    {
        return IsA<T>(object) ? static_cast<T*>(object) : nullptr;
    }

    template <typename T>
    [[nodiscard]] const T* Cast(const Object* object) noexcept
    {
        return IsA<T>(object) ? static_cast<const T*>(object) : nullptr;
    }
}
