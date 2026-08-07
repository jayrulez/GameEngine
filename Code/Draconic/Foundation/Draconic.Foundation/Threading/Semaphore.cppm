// Draconic Foundation - :semaphore partition
//
// Counting semaphore (Mutex + ConditionVariable).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:semaphore;

import :base;
import :mutex;
import :condition_variable;
import :scoped_lock;

export namespace draconic::foundation
{
    class Semaphore
    {
    public:
        explicit Semaphore(i32 initialCount = 0) noexcept : m_count(initialCount) {}

        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;

        void Acquire() noexcept
        {
            ScopedLock lock(m_mutex);
            while (m_count == 0)
            {
                m_available.Wait(m_mutex);
            }
            --m_count;
        }

        [[nodiscard]] bool TryAcquire() noexcept
        {
            ScopedLock lock(m_mutex);
            if (m_count == 0)
            {
                return false;
            }
            --m_count;
            return true;
        }

        void Release() noexcept
        {
            ScopedLock lock(m_mutex);
            ++m_count;
            m_available.NotifyOne();
        }

    private:
        Mutex m_mutex;
        ConditionVariable m_available;
        i32 m_count;
    };
}
