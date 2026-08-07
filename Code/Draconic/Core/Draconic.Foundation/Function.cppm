// Draconic Foundation - :function partition
//
// Function<R(Args...)>: a move-only, type-erased callable (the engine's delegate
// type). Holds a function pointer, a lambda (with captures), or any callable.
// Small callables live inline (small-buffer optimization); larger ones fall back
// to an allocator. No exceptions, no RTTI - invoking an empty Function asserts.
//
// Move-only by design: a stored callable need not be copyable, and ownership is
// unambiguous. Store it in an Array, pass it to a job, hand it to a render pass.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include <type_traits>

export module draconic.foundation:function;

import :base;
import :allocator;

export namespace draconic::foundation
{
    template <typename Signature>
    class Function; // primary template undefined; only R(Args...) is valid

    template <typename R, typename... Args>
    class Function<R(Args...)>
    {
    public:
        Function() noexcept = default;
        Function(decltype(nullptr)) noexcept {}

        // Construct from any callable invocable as R(Args...).
        template <typename F>
            requires(!std::is_same_v<std::decay_t<F>, Function> &&
                     std::is_invocable_r_v<R, std::decay_t<F>&, Args...>)
        Function(F&& callable, IAllocator& allocator = DefaultAllocator())
        {
            using Target = std::decay_t<F>;

            if constexpr (sizeof(Target) <= kInlineSize && alignof(Target) <= kInlineAlign &&
                          std::is_nothrow_move_constructible_v<Target>)
            {
                m_object = ::new (static_cast<void*>(&m_storage)) Target(Forward<F>(callable));
                m_move = [](void* dst, void* src) noexcept
                {
                    Target* s = static_cast<Target*>(src);
                    ::new (dst) Target(Move(*s));
                    s->~Target();
                };
            }
            else
            {
                void* memory = allocator.Allocate(sizeof(Target), alignof(Target));
                DRACONIC_ASSERT_MSG(memory != nullptr, "Function allocation failed");
                m_object = ::new (memory) Target(Forward<F>(callable));
                m_allocator = &allocator;
            }

            m_invoke = [](void* obj, Args&&... args) -> R
            { return (*static_cast<Target*>(obj))(static_cast<Args&&>(args)...); };
            m_destroy = [](void* obj) noexcept { static_cast<Target*>(obj)->~Target(); };
        }

        Function(Function&& other) noexcept { MoveFrom(other); }

        Function& operator=(Function&& other) noexcept
        {
            if (this != &other)
            {
                Destroy();
                MoveFrom(other);
            }
            return *this;
        }

        Function& operator=(decltype(nullptr)) noexcept
        {
            Destroy();
            return *this;
        }

        Function(const Function&) = delete;
        Function& operator=(const Function&) = delete;

        ~Function() { Destroy(); }

        [[nodiscard]] explicit operator bool() const noexcept { return m_invoke != nullptr; }

        R operator()(Args... args) const
        {
            DRACONIC_ASSERT_MSG(m_invoke != nullptr, "Called an empty Function");
            return m_invoke(m_object, static_cast<Args&&>(args)...);
        }

        void Reset() noexcept { Destroy(); }

    private:
        static constexpr usize kInlineSize = 3 * sizeof(void*);
        static constexpr usize kInlineAlign = alignof(void*) > 16 ? alignof(void*) : 16;

        using InvokeFn = R (*)(void*, Args&&...);
        using MoveFn = void (*)(void* dst, void* src) noexcept; // SBO targets only
        using DestroyFn = void (*)(void*) noexcept;

        void Destroy() noexcept
        {
            if (m_object != nullptr)
            {
                m_destroy(m_object);
                if (m_allocator != nullptr)
                {
                    m_allocator->Free(m_object);
                }
            }
            m_object = nullptr;
            m_allocator = nullptr;
            m_invoke = nullptr;
            m_move = nullptr;
            m_destroy = nullptr;
        }

        void MoveFrom(Function& other) noexcept
        {
            if (other.m_object == nullptr)
            {
                return;
            }

            m_invoke = other.m_invoke;
            m_destroy = other.m_destroy;

            if (other.m_allocator != nullptr)
            {
                // Heap target: steal the pointer; no per-object move needed.
                m_object = other.m_object;
                m_allocator = other.m_allocator;
            }
            else
            {
                // Inline target: relocate into our buffer.
                m_move = other.m_move;
                m_move(static_cast<void*>(&m_storage), other.m_object);
                m_object = static_cast<void*>(&m_storage);
            }

            other.m_object = nullptr;
            other.m_allocator = nullptr;
            other.m_invoke = nullptr;
            other.m_move = nullptr;
            other.m_destroy = nullptr;
        }

        alignas(kInlineAlign) unsigned char m_storage[kInlineSize];
        void* m_object = nullptr;          // -> &m_storage (inline) or heap
        IAllocator* m_allocator = nullptr; // non-null => heap allocation owned
        InvokeFn m_invoke = nullptr;
        MoveFn m_move = nullptr;
        DestroyFn m_destroy = nullptr;
    };
}
