// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :object partition
//
// Object: the polymorphic reflection root (derives RefCounted), plus the
// Cast/IsA helpers that replace dynamic_cast by walking the base chain.

module;
#include "Core/Prelude.h"

export module foundation.core:object;

import :base;
import :ref_counted;
import :type_info;

export namespace foundation::core
{
    // =======================================================================
    // Object - polymorphic reflection root. Derives from RefCounted, so every
    // Object is held via RefPtr<Object>.
    // =======================================================================
    class Object : public RefCounted
    {
    public:
        using Super = void;

        [[nodiscard]] virtual const TypeInfo* GetType() const noexcept { return &StaticType(); }

        // Not inline (defined in RefCountedImpl.cpp): this is the root of every base chain,
        // and an in-class body is implicitly inline and would be duplicated per shared
        // library. Id compares keep lookups correct either way; one definition keeps chain
        // walks cheap and the metadata single-instance.
        [[nodiscard]] static const TypeInfo& StaticType() noexcept;
    };

    // =======================================================================
    // Cast / IsA - replace dynamic_cast by walking the single-inheritance chain.
    // Comparison is by TypeId, not pointer: each shared library holds its own copy
    // of vague-linkage TypeInfo statics, so addresses diverge across boundaries
    // while ids do not. The chain pointers are still used: they are internally
    // consistent within whichever library built them.
    // =======================================================================
    [[nodiscard]] inline bool IsDerivedFrom(const TypeInfo* type, const TypeInfo* base) noexcept
    {
        if (base == nullptr)
        {
            return false;
        }
        for (const TypeInfo* t = type; t != nullptr; t = t->base)
        {
            if (t == base || t->id == base->id)
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
