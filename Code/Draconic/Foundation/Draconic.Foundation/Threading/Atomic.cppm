// Draconic Foundation - :atomic partition
//
// Atomic<T> aliases the language <atomic>.

module;
#include "Draconic.Foundation/Prelude.h"
#include <atomic>

export module draconic.foundation:atomic;

export namespace draconic::foundation
{
    template <typename T>
    using Atomic = std::atomic<T>;
}
