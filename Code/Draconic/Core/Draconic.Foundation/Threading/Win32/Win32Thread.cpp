// Draconic Foundation - Threading backend, Win32 implementation.
//
// NOTE: written against ThreadBackend.h for Windows/MSVC; not compiled in the
// Linux dev environment. Validate on Windows. Mutex uses CRITICAL_SECTION and
// ConditionVariable uses CONDITION_VARIABLE (both stored in the opaque buffers).

#include "Draconic.Foundation/Threading/ThreadBackend.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdlib>

namespace draconic::foundation::sys
{
    static_assert(sizeof(CRITICAL_SECTION) <= kMutexStorageSize, "kMutexStorageSize too small");
    static_assert(alignof(CRITICAL_SECTION) <= kMutexStorageAlign, "kMutexStorageAlign too small");
    static_assert(sizeof(CONDITION_VARIABLE) <= kCondStorageSize, "kCondStorageSize too small");
    static_assert(alignof(CONDITION_VARIABLE) <= kCondStorageAlign, "kCondStorageAlign too small");

    namespace
    {
        struct TrampolineData
        {
            void (*entry)(void*);
            void* arg;
        };

        DWORD WINAPI ThreadTrampoline(LPVOID raw)
        {
            TrampolineData data = *static_cast<TrampolineData*>(raw);
            std::free(raw);
            data.entry(data.arg);
            return 0;
        }

        CRITICAL_SECTION* AsCriticalSection(void* storage) noexcept
        {
            return static_cast<CRITICAL_SECTION*>(storage);
        }
        CONDITION_VARIABLE* AsConditionVariable(void* storage) noexcept
        {
            return static_cast<CONDITION_VARIABLE*>(storage);
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

        HANDLE handle = CreateThread(nullptr, 0, ThreadTrampoline, data, 0, nullptr);
        if (handle == nullptr)
        {
            std::free(data);
            return kInvalidThread;
        }
        return reinterpret_cast<ThreadHandle>(handle);
    }

    void ThreadJoin(ThreadHandle handle) noexcept
    {
        HANDLE h = reinterpret_cast<HANDLE>(handle);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }

    void ThreadDetach(ThreadHandle handle) noexcept
    {
        CloseHandle(reinterpret_cast<HANDLE>(handle));
    }

    std::uint64_t CurrentThreadId() noexcept
    {
        return static_cast<std::uint64_t>(GetCurrentThreadId());
    }

    void MutexInit(void* storage) noexcept
    {
        InitializeCriticalSection(AsCriticalSection(storage));
    }
    void MutexDestroy(void* storage) noexcept { DeleteCriticalSection(AsCriticalSection(storage)); }
    void MutexLock(void* storage) noexcept { EnterCriticalSection(AsCriticalSection(storage)); }
    bool MutexTryLock(void* storage) noexcept
    {
        return TryEnterCriticalSection(AsCriticalSection(storage)) != 0;
    }
    void MutexUnlock(void* storage) noexcept { LeaveCriticalSection(AsCriticalSection(storage)); }

    void CondInit(void* storage) noexcept
    {
        InitializeConditionVariable(AsConditionVariable(storage));
    }
    void CondDestroy(void* /*storage*/) noexcept { /* CONDITION_VARIABLE needs no destruction */ }
    void CondWait(void* cond, void* mutex) noexcept
    {
        SleepConditionVariableCS(AsConditionVariable(cond), AsCriticalSection(mutex), INFINITE);
    }
    void CondSignal(void* storage) noexcept { WakeConditionVariable(AsConditionVariable(storage)); }
    void CondBroadcast(void* storage) noexcept
    {
        WakeAllConditionVariable(AsConditionVariable(storage));
    }
}
