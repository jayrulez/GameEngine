// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Validation wrapper for Adapter.

module;
#include "Core/Prelude.h"

export module foundation.rhi.validation:validated_adapter;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::validation
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

} // namespace foundation::rhi::validation
