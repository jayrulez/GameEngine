// Draconic Foundation - :instance partition
//
// Instance: a borrowed, type-erased { void*, TypeInfo* } target for member
// access (the `this` of a reflected property/method call). Non-owning.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:instance;

import :base;
import :type_info;

export namespace draconic::foundation
{
    // =======================================================================
    // Instance - a borrowed, type-erased pointer to a live object.
    // =======================================================================
    class Instance
    {
    public:
        Instance() noexcept = default;
        Instance(void* pointer, const TypeInfo* type) noexcept : m_ptr(pointer), m_type(type) {}

        template <typename T>
        [[nodiscard]] static Instance From(T* pointer) noexcept
        {
            return Instance{pointer, &TypeOf<T>()};
        }

        [[nodiscard]] bool IsEmpty() const noexcept { return m_ptr == nullptr; }
        [[nodiscard]] void* Pointer() const noexcept { return m_ptr; }
        [[nodiscard]] const TypeInfo* Type() const noexcept { return m_type; }

        template <typename T>
        [[nodiscard]] T* TryGet() const noexcept
        {
            return (m_type == &TypeOf<T>()) ? static_cast<T*>(m_ptr) : nullptr;
        }

    private:
        void* m_ptr = nullptr;
        const TypeInfo* m_type = nullptr;
    };
}
