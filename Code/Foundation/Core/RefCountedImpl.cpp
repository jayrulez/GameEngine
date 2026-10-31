// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The slot MakeRef uses to hand the control block to the RefCounted constructor. It is
// defined here, in Core's one implementation unit, so the whole process shares a single
// slot per thread: MakeRef<T> instantiates into the calling library while the constructed
// type may live in another, and an inline thread_local would be duplicated per shared
// library, leaving m_control null on every object built across a boundary.

module;
#include "Core/Prelude.h"

module foundation.core;

namespace foundation::core
{
    // The Object hierarchy root's TypeInfo (declared in Object.cppm).
    const TypeInfo& Object::StaticType() noexcept
    {
        static const TypeInfo info{ComputeTypeId("rtti::core", "Object"),
                                   "Object",
                                   "rtti::core",
                                   static_cast<u32>(sizeof(Object)),
                                   static_cast<u32>(alignof(Object)),
                                   nullptr};
        return info;
    }

    // Reflection borrow-invalidation generation (declared in Variant.cppm).
    u64& ReflectionMutationGenerationRef() noexcept
    {
        static u64 generation = 1; // start at 1 so a default (0) borrow generation never matches
        return generation;
    }

    namespace detail
    {
        // Memory-tag name registry (declared in MemoryTag.cppm).
        MemoryTagRegistry& MemoryTags() noexcept
        {
            static MemoryTagRegistry registry;
            return registry;
        }
    } // namespace detail
} // namespace foundation::core

namespace foundation::core::detail
{
    RefControl*& PendingRefControl() noexcept
    {
        static thread_local RefControl* slot = nullptr;
        return slot;
    }
} // namespace foundation::core::detail
