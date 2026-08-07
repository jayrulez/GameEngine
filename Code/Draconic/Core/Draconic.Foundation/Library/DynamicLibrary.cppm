// Draconic Foundation - :library partition
//
// DynamicLibrary: an RAII handle over the System raw dynamic-library calls,
// with typed symbol resolution. Foundation for the future plugin/module system
// (discover, load, init/shutdown lifecycle, hot-reload) - S4.9.

module;
#include "Draconic.Foundation/Prelude.h"
#include <cstring> // memcpy (avoids the pedantic void*->function-pointer cast)

export module draconic.foundation:library;

import :base;
import :string;
import :system;

export namespace draconic::foundation
{
    class DynamicLibrary
    {
    public:
        DynamicLibrary() noexcept = default;
        explicit DynamicLibrary(StringView path) noexcept { m_handle = OpenLibrary(path); }

        DynamicLibrary(DynamicLibrary&& other) noexcept : m_handle(other.m_handle)
        {
            other.m_handle = nullptr;
        }

        DynamicLibrary& operator=(DynamicLibrary&& other) noexcept
        {
            if (this != &other)
            {
                Unload();
                m_handle = other.m_handle;
                other.m_handle = nullptr;
            }
            return *this;
        }

        DynamicLibrary(const DynamicLibrary&) = delete;
        DynamicLibrary& operator=(const DynamicLibrary&) = delete;

        ~DynamicLibrary() { Unload(); }

        Status Load(StringView path) noexcept
        {
            Unload();
            m_handle = OpenLibrary(path);
            return IsLoaded() ? Status{} : Status{ErrorCode::NotFound};
        }

        void Unload() noexcept
        {
            if (m_handle != nullptr)
            {
                CloseLibrary(m_handle);
                m_handle = nullptr;
            }
        }

        [[nodiscard]] bool IsLoaded() const noexcept { return m_handle != nullptr; }

        // Resolves a symbol as the requested pointer type (typically a function
        // pointer). Returns nullptr if absent or not loaded.
        template <typename T>
        [[nodiscard]] T GetSymbol(StringView name) const noexcept
        {
            static_assert(sizeof(T) == sizeof(void*), "GetSymbol<T> expects a pointer type.");
            void* symbol = (m_handle != nullptr) ? GetLibrarySymbol(m_handle, name) : nullptr;

            T result{};
            std::memcpy(&result, &symbol, sizeof(result));
            return result;
        }

    private:
        LibraryHandle m_handle = nullptr;
    };
}
