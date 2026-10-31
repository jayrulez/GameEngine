// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Validation wrapper for Backend.

module;
#include "Core/Prelude.h"

export module foundation.rhi.validation:validated_backend;

import foundation.core;
import foundation.rhi;
import :validated_adapter;

using namespace foundation::core;

export namespace foundation::rhi::validation
{

    class ValidatedBackend : public Backend
    {
    public:
        explicit ValidatedBackend(Backend* inner, IAllocator& allocator)
            : m_inner(inner), m_allocator(allocator)
        {
            isInitialized = inner->isInitialized;
        }

        Span<Adapter* const> EnumerateAdapters() override
        {
            if (!m_inner->isInitialized)
            {
                LogError("[Validation] enumerateAdapters: backend not initialized");
                return {};
            }

            if (m_adapterWrappers.IsEmpty())
            {
                auto innerAdapters = m_inner->EnumerateAdapters();
                m_adapterWrappers.Reserve(innerAdapters.Size());
                m_adapterPtrs.Reserve(innerAdapters.Size());
                for (usize i = 0; i < innerAdapters.Size(); ++i)
                {
                    auto* w = CreateValidatedAdapter(innerAdapters[i], m_allocator);
                    m_adapterWrappers.PushBack(w);
                    m_adapterPtrs.PushBack(w);
                }
            }
            return Span<Adapter* const>(m_adapterPtrs.Data(), m_adapterPtrs.Size());
        }

        Status CreateSurface(void* windowHandle, void* displayHandle, Surface*& out,
                             SurfacePlatform platform = SurfacePlatform::Unknown) override
        {
            if (!windowHandle)
            {
                LogError("[Validation] createSurface: windowHandle is null");
                out = nullptr;
                return ErrorCode::InvalidArgument;
            }
            return m_inner->CreateSurface(windowHandle, displayHandle, out, platform);
        }

        void Destroy() override
        {
            for (auto* w : m_adapterWrappers)
                m_allocator.Delete(w);
            m_adapterWrappers.Clear();
            m_adapterPtrs.Clear();
            m_inner->Destroy();
            IAllocator& alloc = m_allocator;
            this->~ValidatedBackend();
            alloc.Free(this);
        }

        Backend* inner() const { return m_inner; }

    private:
        static ValidatedAdapter* CreateValidatedAdapter(Adapter* inner, IAllocator& allocator);

        Backend* m_inner;
        IAllocator& m_allocator;
        Array<ValidatedAdapter*> m_adapterWrappers;
        Array<Adapter*> m_adapterPtrs;
    };

    Backend* CreateValidatedBackend(Backend* inner, IAllocator& allocator = DefaultAllocator())
    {
        if (!inner)
        {
            LogError("[Validation] CreateValidatedBackend: inner is null");
            return nullptr;
        }
        return allocator.New<ValidatedBackend>(inner, allocator);
    }

} // namespace foundation::rhi::validation
