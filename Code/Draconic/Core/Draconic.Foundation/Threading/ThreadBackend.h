// Draconic Foundation - Threading backend (classic header).
//
// OS threading primitives implemented per-platform (Threading/Linux,
// Threading/Win32). Mutex/condition storage is an opaque fixed buffer so the
// module never sees pthread.h / windows.h. Sizes are conservative and checked
// with a static_assert in the platform .cpp.

#ifndef DRACONIC_FOUNDATION_THREADING_BACKEND_H
#define DRACONIC_FOUNDATION_THREADING_BACKEND_H

#include <cstddef>
#include <cstdint>

namespace draconic::foundation::sys
{
    // --- Threads -----------------------------------------------------------
    using ThreadHandle = std::uintptr_t;
    inline constexpr ThreadHandle kInvalidThread = 0;

    ThreadHandle ThreadCreate(void (*entry)(void*),
                              void* arg) noexcept; // kInvalidThread on failure
    void ThreadJoin(ThreadHandle handle) noexcept;
    void ThreadDetach(ThreadHandle handle) noexcept;
    [[nodiscard]] std::uint64_t CurrentThreadId() noexcept;

    // --- Mutex (opaque storage) -------------------------------------------
    inline constexpr std::size_t kMutexStorageSize = 64;
    inline constexpr std::size_t kMutexStorageAlign = 16;

    void MutexInit(void* storage) noexcept;
    void MutexDestroy(void* storage) noexcept;
    void MutexLock(void* storage) noexcept;
    [[nodiscard]] bool MutexTryLock(void* storage) noexcept;
    void MutexUnlock(void* storage) noexcept;

    // --- Condition variable (opaque storage) ------------------------------
    inline constexpr std::size_t kCondStorageSize = 64;
    inline constexpr std::size_t kCondStorageAlign = 16;

    void CondInit(void* storage) noexcept;
    void CondDestroy(void* storage) noexcept;
    void CondWait(void* cond, void* mutex) noexcept; // mutex must be held
    void CondSignal(void* storage) noexcept;
    void CondBroadcast(void* storage) noexcept;
}

#endif // DRACONIC_FOUNDATION_THREADING_BACKEND_H
