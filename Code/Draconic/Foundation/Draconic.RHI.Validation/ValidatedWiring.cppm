/// Deferred implementations that break circular dependencies between
/// ValidatedBackend, ValidatedAdapter, and ValidatedDevice.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rhi.validation:wiring;

import draconic.foundation;
import draconic.rhi;
import :validated_backend;
import :validated_adapter;
import :validated_device;

using namespace draconic::foundation;

namespace draconic::rhi::validation
{

    ValidatedAdapter* ValidatedBackend::CreateValidatedAdapter(Adapter* inner,
                                                               IAllocator& allocator)
    {
        return allocator.New<ValidatedAdapter>(inner, allocator);
    }

    Status ValidatedAdapter::CreateDevice(const DeviceDesc& desc, Device*& out)
    {
        Device* innerDevice = nullptr;
        Status r = m_inner->CreateDevice(desc, innerDevice);
        if (r != ErrorCode::Ok || !innerDevice)
        {
            out = nullptr;
            return r;
        }
        out = m_allocator.New<ValidatedDevice>(innerDevice, m_allocator);
        return ErrorCode::Ok;
    }

} // namespace draconic::rhi::validation
