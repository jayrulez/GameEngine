// Draconic Foundation - Threading backend, Linux (pthreads) implementation.

#include "Draconic.Foundation/Threading/ThreadBackend.h"

#include <cstdlib>
#include <pthread.h>

namespace draconic::foundation::sys
{
    static_assert(sizeof(pthread_mutex_t) <= kMutexStorageSize, "kMutexStorageSize too small");
    static_assert(alignof(pthread_mutex_t) <= kMutexStorageAlign, "kMutexStorageAlign too small");
    static_assert(sizeof(pthread_cond_t) <= kCondStorageSize, "kCondStorageSize too small");
    static_assert(alignof(pthread_cond_t) <= kCondStorageAlign, "kCondStorageAlign too small");

    namespace
    {
        struct TrampolineData
        {
            void (*entry)(void*);
            void* arg;
        };

        void* PthreadTrampoline(void* raw)
        {
            TrampolineData data = *static_cast<TrampolineData*>(raw);
            std::free(raw);
            data.entry(data.arg);
            return nullptr;
        }

        pthread_mutex_t* AsMutex(void* storage) noexcept
        {
            return static_cast<pthread_mutex_t*>(storage);
        }
        pthread_cond_t* AsCond(void* storage) noexcept
        {
            return static_cast<pthread_cond_t*>(storage);
        }
    }

    ThreadHandle ThreadCreate(void (*entry)(void*), void* arg) noexcept
    {
        auto* data = static_cast<TrampolineData*>(std::malloc(sizeof(TrampolineData)));
        if (data == nullptr)
        {
            return kInvalidThread;
        }
        data->entry = entry;
        data->arg = arg;

        pthread_t thread{};
        if (pthread_create(&thread, nullptr, PthreadTrampoline, data) != 0)
        {
            std::free(data);
            return kInvalidThread;
        }
        return static_cast<ThreadHandle>(thread);
    }

    void ThreadJoin(ThreadHandle handle) noexcept
    {
        pthread_join(static_cast<pthread_t>(handle), nullptr);
    }

    void ThreadDetach(ThreadHandle handle) noexcept
    {
        pthread_detach(static_cast<pthread_t>(handle));
    }

    std::uint64_t CurrentThreadId() noexcept { return static_cast<std::uint64_t>(pthread_self()); }

    void MutexInit(void* storage) noexcept { pthread_mutex_init(AsMutex(storage), nullptr); }
    void MutexDestroy(void* storage) noexcept { pthread_mutex_destroy(AsMutex(storage)); }
    void MutexLock(void* storage) noexcept { pthread_mutex_lock(AsMutex(storage)); }
    bool MutexTryLock(void* storage) noexcept
    {
        return pthread_mutex_trylock(AsMutex(storage)) == 0;
    }
    void MutexUnlock(void* storage) noexcept { pthread_mutex_unlock(AsMutex(storage)); }

    void CondInit(void* storage) noexcept { pthread_cond_init(AsCond(storage), nullptr); }
    void CondDestroy(void* storage) noexcept { pthread_cond_destroy(AsCond(storage)); }
    void CondWait(void* cond, void* mutex) noexcept
    {
        pthread_cond_wait(AsCond(cond), AsMutex(mutex));
    }
    void CondSignal(void* storage) noexcept { pthread_cond_signal(AsCond(storage)); }
    void CondBroadcast(void* storage) noexcept { pthread_cond_broadcast(AsCond(storage)); }
}
