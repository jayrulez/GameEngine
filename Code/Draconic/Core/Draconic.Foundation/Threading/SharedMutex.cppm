// Draconic Foundation - :shared_mutex partition
//
// Writer-preferring read/write lock + ScopedSharedLock guard.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:shared_mutex;

import :base;
import :mutex;
import :condition_variable;
import :scoped_lock;

export namespace draconic::foundation
{
    class SharedMutex
    {
    public:
        SharedMutex() noexcept = default;
        SharedMutex(const SharedMutex&) = delete;
        SharedMutex& operator=(const SharedMutex&) = delete;

        void Lock() noexcept // exclusive
        {
            ScopedLock lock(m_mutex);
            ++m_writersWaiting;
            while (m_writeActive || m_readers > 0)
            {
                m_gate.Wait(m_mutex);
            }
            --m_writersWaiting;
            m_writeActive = true;
        }

        void Unlock() noexcept
        {
            ScopedLock lock(m_mutex);
            m_writeActive = false;
            m_gate.NotifyAll();
        }

        void LockShared() noexcept // read
        {
            ScopedLock lock(m_mutex);
            while (m_writeActive || m_writersWaiting > 0)
            {
                m_gate.Wait(m_mutex);
            }
            ++m_readers;
        }

        void UnlockShared() noexcept
        {
            ScopedLock lock(m_mutex);
            if (--m_readers == 0)
            {
                m_gate.NotifyAll();
            }
        }

    private:
        Mutex m_mutex;
        ConditionVariable m_gate;
        i32 m_readers = 0;
        i32 m_writersWaiting = 0;
        bool m_writeActive = false;
    };

    class ScopedSharedLock
    {
    public:
        explicit ScopedSharedLock(SharedMutex& mutex) noexcept : m_mutex(&mutex)
        {
            m_mutex->LockShared();
        }
        ~ScopedSharedLock() { m_mutex->UnlockShared(); }

        ScopedSharedLock(const ScopedSharedLock&) = delete;
        ScopedSharedLock& operator=(const ScopedSharedLock&) = delete;

    private:
        SharedMutex* m_mutex;
    };
}
