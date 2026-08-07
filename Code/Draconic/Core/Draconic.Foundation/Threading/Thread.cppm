// Draconic Foundation - :thread partition
//
// Thread owns an OS thread running a callable; join or detach before destroy.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include "Draconic.Foundation/Threading/ThreadBackend.h"

export module draconic.foundation:thread;

import :base;
import :allocator;

export namespace draconic::foundation
{
    class Thread
    {
    public:
        Thread() noexcept = default;

        template <typename Fn>
        explicit Thread(Fn function)
        {
            Fn* held = DefaultAllocator().New<Fn>(Move(function));
            m_handle = sys::ThreadCreate(&ThreadEntry<Fn>, held);
            m_joinable = (m_handle != sys::kInvalidThread);
            if (!m_joinable)
            {
                DefaultAllocator().Delete(held);
            }
        }

        Thread(Thread&& other) noexcept : m_handle(other.m_handle), m_joinable(other.m_joinable)
        {
            other.m_handle = sys::kInvalidThread;
            other.m_joinable = false;
        }

        Thread& operator=(Thread&& other) noexcept
        {
            if (this != &other)
            {
                DRACONIC_ASSERT_MSG(!m_joinable, "Thread overwritten while still joinable");
                m_handle = other.m_handle;
                m_joinable = other.m_joinable;
                other.m_handle = sys::kInvalidThread;
                other.m_joinable = false;
            }
            return *this;
        }

        Thread(const Thread&) = delete;
        Thread& operator=(const Thread&) = delete;

        ~Thread()
        {
            if (m_joinable)
            {
                DRACONIC_ASSERT_MSG(false,
                                    "Thread destroyed while still joinable; call Join or Detach");
                sys::ThreadDetach(m_handle);
            }
        }

        [[nodiscard]] bool IsJoinable() const noexcept { return m_joinable; }

        void Join() noexcept
        {
            if (m_joinable)
            {
                sys::ThreadJoin(m_handle);
                m_joinable = false;
            }
        }

        void Detach() noexcept
        {
            if (m_joinable)
            {
                sys::ThreadDetach(m_handle);
                m_joinable = false;
            }
        }

        [[nodiscard]] static u64 CurrentId() noexcept { return sys::CurrentThreadId(); }

    private:
        template <typename Fn>
        static void ThreadEntry(void* arg)
        {
            Fn* function = static_cast<Fn*>(arg);
            (*function)();
            DefaultAllocator().Delete(function);
        }

        sys::ThreadHandle m_handle = sys::kInvalidThread;
        bool m_joinable = false;
    };
}
