// Draconic RHI - :resources implementation unit.
//
// Out-of-line bodies whose headers must stay out of the interface (GCC gcm-cluster
// hygiene): currently just the TextureView unique-id counter (<atomic>).

module;
#include <atomic>

module draconic.rhi;

import draconic.foundation;

namespace draconic::rhi
{
    foundation::u64 NextTextureViewUniqueId() noexcept
    {
        static std::atomic<foundation::u64> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }
}
