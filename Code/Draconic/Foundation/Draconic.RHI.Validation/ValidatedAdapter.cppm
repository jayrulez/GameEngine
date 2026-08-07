/// Validation wrapper for Adapter.
/// Ported from Sedulous.RHI.Validation/ValidatedAdapter.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rhi.validation:validated_adapter;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::validation
{

    class ValidatedDevice;

    class ValidatedAdapter : public Adapter
    {
    public:
        explicit ValidatedAdapter(Adapter* inner, IAllocator& allocator)
            : m_inner(inner), m_allocator(allocator)
        {
        }

        void GetInfo(AdapterInfo& out) override { m_inner->GetInfo(out); }

        Status CreateDevice(const DeviceDesc& desc, Device*& out) override;

        Adapter* inner() const { return m_inner; }

    private:
        Adapter* m_inner;
        IAllocator& m_allocator;
    };

} // namespace draconic::rhi::validation
