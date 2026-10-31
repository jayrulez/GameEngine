// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Validation wrapper for TransferBatch.

module;
#include "Core/Prelude.h"

export module foundation.rhi.validation:validated_transfer_batch;

import foundation.core;
import foundation.rhi;
import :validated_fence;

using namespace foundation::core;

export namespace foundation::rhi::validation
{

    class ValidatedTransferBatch : public TransferBatch
    {
    public:
        explicit ValidatedTransferBatch(TransferBatch* inner) : m_inner(inner) {}

        void WriteBuffer(Buffer* dst, u64 dstOffset, Span<const u8> data) override
        {
            if (m_destroyed)
            {
                LogError("[Validation] TransferBatch::writeBuffer: batch already destroyed");
                return;
            }
            if (!dst)
            {
                LogError("[Validation] TransferBatch::writeBuffer: dst is null");
                return;
            }
            if (data.Size() == 0)
            {
                LogWarning("[Validation] TransferBatch::writeBuffer: data is empty");
                return;
            }
            m_pendingWrites++;
            m_inner->WriteBuffer(dst, dstOffset, data);
        }

        void WriteTexture(Texture* dst, Span<const u8> data, const TextureDataLayout& layout,
                          Extent3D extent, u32 mipLevel, u32 arrayLayer) override
        {
            if (m_destroyed)
            {
                LogError("[Validation] TransferBatch::writeTexture: batch already destroyed");
                return;
            }
            if (!dst)
            {
                LogError("[Validation] TransferBatch::writeTexture: dst is null");
                return;
            }
            if (data.Size() == 0)
            {
                LogWarning("[Validation] TransferBatch::writeTexture: data is empty");
                return;
            }
            if (extent.width == 0 || extent.height == 0)
            {
                LogError("[Validation] TransferBatch::writeTexture: extent is zero");
                return;
            }
            m_pendingWrites++;
            m_inner->WriteTexture(dst, data, layout, extent, mipLevel, arrayLayer);
        }

        Status Submit() override
        {
            if (m_destroyed)
            {
                LogError("[Validation] TransferBatch::submit: batch already destroyed");
                return ErrorCode::Unknown;
            }
            if (m_pendingWrites == 0)
                LogWarning("[Validation] TransferBatch::submit: no pending writes");
            m_pendingWrites = 0;
            return m_inner->Submit();
        }

        Status SubmitAsync(Fence* fence, u64 signalValue) override
        {
            if (m_destroyed)
            {
                LogError("[Validation] TransferBatch::submitAsync: batch already destroyed");
                return ErrorCode::Unknown;
            }
            if (!fence)
            {
                LogError("[Validation] TransferBatch::submitAsync: fence is null");
                return ErrorCode::Unknown;
            }
            if (m_pendingWrites == 0)
                LogWarning("[Validation] TransferBatch::submitAsync: no pending writes");
            m_pendingWrites = 0;
            auto* vf = static_cast<ValidatedFence*>(fence);
            Fence* innerFence = vf ? vf->inner() : fence;
            if (vf)
                vf->trackSignal(signalValue);
            return m_inner->SubmitAsync(innerFence, signalValue);
        }

        void Reset() override
        {
            m_pendingWrites = 0;
            m_inner->Reset();
        }

        void Destroy() override
        {
            if (m_destroyed)
            {
                LogWarning("[Validation] TransferBatch::destroy: already destroyed");
                return;
            }
            m_destroyed = true;
            m_inner->Destroy();
        }

        TransferBatch* inner() const { return m_inner; }

    private:
        TransferBatch* m_inner;
        bool m_destroyed = false;
        i32 m_pendingWrites = 0;
    };

} // namespace foundation::rhi::validation
