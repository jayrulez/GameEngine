// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Out-of-line PickSystem definitions (see PickSystem.cppm).
module;
#include "Core/Prelude.h"
module foundation.render;
import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import :data;
import :resources;
import :picking;
using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace rhi = foundation::rhi;

namespace foundation::render
{
    bool ClampPickRect(PickRect& rect, u32 viewportWidth, u32 viewportHeight) noexcept
    {
        if (viewportWidth == 0 || viewportHeight == 0 || rect.width == 0 || rect.height == 0)
        {
            return false;
        }
        const i64 x0 = Max<i64>(rect.x, 0);
        const i64 y0 = Max<i64>(rect.y, 0);
        const i64 x1 = Min<i64>(static_cast<i64>(rect.x) + static_cast<i64>(rect.width),
                                static_cast<i64>(viewportWidth));
        const i64 y1 = Min<i64>(static_cast<i64>(rect.y) + static_cast<i64>(rect.height),
                                static_cast<i64>(viewportHeight));
        if (x1 <= x0 || y1 <= y0)
        {
            return false;
        }
        rect.x = static_cast<i32>(x0);
        rect.y = static_cast<i32>(y0);
        rect.width = static_cast<u32>(x1 - x0);
        rect.height = static_cast<u32>(y1 - y0);
        return true;
    }

    Float4x4 CropProjectionToRect(const Float4x4& projection, const PickRect& rect,
                                  u32 viewportWidth, u32 viewportHeight) noexcept
    {
        if (viewportWidth == 0 || viewportHeight == 0 || rect.width == 0 || rect.height == 0)
        {
            return projection;
        }
        const f32 fullW = static_cast<f32>(viewportWidth);
        const f32 fullH = static_cast<f32>(viewportHeight);
        // Rect centre in pixels, then in NDC (x right, y up: pixel row 0 is the top of the view).
        const f32 cx = static_cast<f32>(rect.x) + static_cast<f32>(rect.width) * 0.5f;
        const f32 cy = static_cast<f32>(rect.y) + static_cast<f32>(rect.height) * 0.5f;
        const f32 ndcCx = 2.0f * cx / fullW - 1.0f;
        const f32 ndcCy = 1.0f - 2.0f * cy / fullH;
        const f32 sx = fullW / static_cast<f32>(rect.width);
        const f32 sy = fullH / static_cast<f32>(rect.height);
        // clip' = clip * crop: x' = sx * x - sx * ndcCx * w, y' likewise; z and w untouched.
        // Scaling x and y and adding multiples of w commutes with the perspective divide, so the
        // divided result is exactly the NDC remap (ndc - centre) * scale.
        Float4x4 crop = Float4x4::Identity();
        crop(0, 0) = sx;
        crop(1, 1) = sy;
        crop(3, 0) = -sx * ndcCx;
        crop(3, 1) = -sy * ndcCy;
        return projection * crop;
    }

    u32 PickRowStride(u32 width) noexcept
    {
        const u32 tight = width * kPickTexelBytes;
        return (tight + (kPickRowAlignment - 1u)) & ~(kPickRowAlignment - 1u);
    }

    void DecodePickTexels(const u8* rows, u32 width, u32 height, u32 rowStrideBytes,
                          Array<PickHit>& out)
    {
        out.Clear();
        if (rows == nullptr || width == 0 || height == 0)
        {
            return;
        }
        for (u32 y = 0; y < height; ++y)
        {
            const u8* row = rows + static_cast<usize>(y) * rowStrideBytes;
            for (u32 x = 0; x < width; ++x)
            {
                u32 texel[2];
                MemCopy(texel, row + static_cast<usize>(x) * kPickTexelBytes, sizeof(texel));
                if (texel[0] == 0)
                {
                    continue; // background (the clear)
                }
                const PickHit hit{texel[0] - 1u, texel[1]};
                bool seen = false;
                for (const PickHit& h : out)
                {
                    if (h.entityIndex == hit.entityIndex && h.generation == hit.generation)
                    {
                        seen = true;
                        break;
                    }
                }
                if (!seen)
                {
                    out.PushBack(hit);
                }
            }
        }
    }

    PickSystem::PickSystem(IAllocator& allocator, rhi::Device& device, u32 framesInFlight) noexcept
        : m_allocator(&allocator), m_device(&device),
          m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1u), m_slots(allocator)
    {
    }

    PickSystem::~PickSystem()
    {
        for (Slot& slot : m_slots)
        {
            if (slot.buffer != nullptr)
            {
                // Destruction is a teardown (the owner waited the device idle); free directly.
                m_device->DestroyBuffer(slot.buffer);
                slot.buffer = nullptr;
            }
        }
    }

    PickSystem::Slot* PickSystem::Find(PickRequestId id) noexcept
    {
        if (id == kInvalidPickRequest)
        {
            return nullptr;
        }
        for (Slot& slot : m_slots)
        {
            if (slot.state != Slot::State::Free && slot.id == id)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    const PickSystem::Slot* PickSystem::Find(PickRequestId id) const noexcept
    {
        if (id == kInvalidPickRequest)
        {
            return nullptr;
        }
        for (const Slot& slot : m_slots)
        {
            if (slot.state != Slot::State::Free && slot.id == id)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    PickSystem::Slot& PickSystem::Acquire()
    {
        for (Slot& slot : m_slots)
        {
            if (slot.state == Slot::State::Free)
            {
                return slot;
            }
        }
        Slot fresh;
        fresh.hits = Array<PickHit>(*m_allocator);
        m_slots.PushBack(Move(fresh));
        return m_slots[m_slots.Size() - 1];
    }

    PickRequestId PickSystem::Request(const void* viewportKey, const PickRect& rect)
    {
        Slot& slot = Acquire();
        slot.state = Slot::State::Requested;
        slot.id = m_nextId++;
        if (m_nextId == kInvalidPickRequest)
        {
            m_nextId = 1; // wrapped: 0 stays the invalid id
        }
        slot.viewportKey = viewportKey;
        slot.rect = rect;
        slot.submittedIndex = 0;
        slot.sawOtherIndex = false;
        slot.rendered = false;
        slot.age = 0;
        slot.hits.Clear();
        return slot.id;
    }

    bool PickSystem::TryTakeResult(PickRequestId id, PickResult& out)
    {
        Slot* slot = Find(id);
        if (slot == nullptr || slot->state != Slot::State::Ready)
        {
            return false;
        }
        out.id = id;
        out.rendered = slot->rendered;
        out.hits = Move(slot->hits);
        slot->hits = Array<PickHit>(*m_allocator);
        slot->state = Slot::State::Free;
        slot->id = kInvalidPickRequest;
        slot->viewportKey = nullptr;
        return true;
    }

    bool PickSystem::IsPending(PickRequestId id) const noexcept
    {
        const Slot* slot = Find(id);
        return slot != nullptr && slot->state != Slot::State::Ready;
    }

    u32 PickSystem::PendingCount(const void* viewportKey) const noexcept
    {
        u32 n = 0;
        for (const Slot& slot : m_slots)
        {
            if (slot.state == Slot::State::Requested && slot.viewportKey == viewportKey)
            {
                ++n;
            }
        }
        return n;
    }

    u32 PickSystem::PendingTotal() const noexcept
    {
        u32 n = 0;
        for (const Slot& slot : m_slots)
        {
            if (slot.state == Slot::State::Requested)
            {
                ++n;
            }
        }
        return n;
    }

    void PickSystem::Cancel(const void* viewportKey)
    {
        for (Slot& slot : m_slots)
        {
            if (slot.viewportKey != viewportKey || slot.state == Slot::State::Free)
            {
                continue;
            }
            if (slot.state == Slot::State::AwaitReadback)
            {
                // The copy is in flight: keep the slot (and its buffer) until it retires; the
                // decoded result is thrown away then.
                slot.id = kInvalidPickRequest;
                continue;
            }
            slot.state = Slot::State::Free;
            slot.id = kInvalidPickRequest;
            slot.viewportKey = nullptr;
            slot.hits.Clear();
        }
    }

    bool PickSystem::EnsureBuffer(Slot& slot, u64 bytes)
    {
        if (slot.buffer != nullptr && slot.capacity >= bytes)
        {
            return true;
        }
        ReleaseBuffer(slot);
        rhi::BufferDesc desc{};
        desc.size = bytes;
        desc.usage = rhi::BufferUsage::CopyDst;
        desc.memory = rhi::MemoryLocation::GpuToCpu;
        desc.label = u8"pick.readback";
        if (!m_device->CreateBuffer(desc, slot.buffer).IsOk() || slot.buffer == nullptr)
        {
            slot.buffer = nullptr;
            slot.capacity = 0;
            return false;
        }
        slot.capacity = bytes;
        return true;
    }

    void PickSystem::ReleaseBuffer(Slot& slot)
    {
        if (slot.buffer == nullptr)
        {
            return;
        }
        if (m_retire != nullptr)
        {
            m_retire->Retire(slot.buffer);
        }
        else
        {
            m_device->WaitIdle();
            m_device->DestroyBuffer(slot.buffer);
        }
        slot.buffer = nullptr;
        slot.capacity = 0;
    }

    void PickSystem::BeginFrame(u32 frameIndex)
    {
        for (Slot& slot : m_slots)
        {
            switch (slot.state)
            {
            case Slot::State::AwaitReadback:
            {
                // Retire when the device ring has left and returned to the submission's slot:
                // the submission (and its copy) has provably completed, so the map cannot stall.
                if (frameIndex != slot.submittedIndex)
                {
                    slot.sawOtherIndex = true;
                    break;
                }
                if (!slot.sawOtherIndex)
                {
                    break;
                }
                slot.hits.Clear();
                if (slot.buffer != nullptr)
                {
                    if (const u8* mapped = static_cast<const u8*>(slot.buffer->Map()))
                    {
                        DecodePickTexels(mapped, slot.rect.width, slot.rect.height,
                                         PickRowStride(slot.rect.width), slot.hits);
                        slot.buffer->Unmap();
                    }
                }
                slot.rendered = true;
                if (slot.id == kInvalidPickRequest)
                {
                    slot.state = Slot::State::Free; // cancelled while in flight
                    slot.viewportKey = nullptr;
                    slot.hits.Clear();
                }
                else
                {
                    slot.state = Slot::State::Ready;
                }
                break;
            }
            case Slot::State::Requested:
            {
                if (++slot.age > kExpireFrames)
                {
                    slot.rendered = false;
                    slot.hits.Clear();
                    slot.state = Slot::State::Ready;
                }
                break;
            }
            case Slot::State::Free:
            case Slot::State::Ready:
                break;
            }
        }
    }

    u32 PickSystem::DeclarePasses(rendergraph::RenderGraph& graph, const void* viewportKey,
                                  const Float4x4& view, const Float4x4& projection,
                                  u32 viewportWidth, u32 viewportHeight,
                                  rhi::TextureFormat depthFormat, u32 frameIndex,
                                  PickRecordFn record, void* context, const void* viewContext)
    {
        if (record == nullptr)
        {
            return 0;
        }
        u32 declared = 0;
        for (Slot& slot : m_slots)
        {
            if (slot.state != Slot::State::Requested || slot.viewportKey != viewportKey)
            {
                continue;
            }
            PickRect rect = slot.rect;
            if (!ClampPickRect(rect, viewportWidth, viewportHeight))
            {
                // Nothing of the rect is inside the view: answered without a pass.
                slot.rect = rect;
                slot.hits.Clear();
                slot.rendered = true;
                slot.state = Slot::State::Ready;
                continue;
            }
            const u32 rowStride = PickRowStride(rect.width);
            const u64 bytes = static_cast<u64>(rowStride) * rect.height;
            if (!EnsureBuffer(slot, bytes))
            {
                slot.hits.Clear();
                slot.rendered = false;
                slot.state = Slot::State::Ready;
                continue;
            }
            slot.rect = rect;

            const Float4x4 cropped =
                CropProjectionToRect(projection, rect, viewportWidth, viewportHeight);
            const Float4x4 viewProj = view * cropped;

            rendergraph::RGTextureDesc idDesc(kPickIdFormat, rect.width, rect.height);
            idDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            const rendergraph::RGHandle ids = graph.CreateTransient(u8"pick.ids", idDesc);
            rendergraph::RGTextureDesc depthDesc(depthFormat, rect.width, rect.height);
            depthDesc.usage = rhi::TextureUsage::DepthStencil;
            const rendergraph::RGHandle depth = graph.CreateTransient(u8"pick.depth", depthDesc);
            const rendergraph::RGHandle readback = graph.ImportBuffer(u8"pick.readback", slot.buffer);

            graph.AddRenderPass(
                u8"pick.ids",
                [ids, depth, rect, viewProj, record, context, viewContext](rendergraph::PassBuilder& b)
                {
                    // Uint target: the clear's float zeros are the integer zero (no entity).
                    b.SetColorTarget(0, ids, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                     rhi::ClearColor{0.0f, 0.0f, 0.0f, 0.0f});
                    b.SetDepthTarget(depth, rhi::LoadOp::Clear, rhi::StoreOp::DontCare);
                    b.SetViewport(0, 0, rect.width, rect.height);
                    b.NeverCull();
                    b.SetExecute([record, context, viewProj, rect, viewContext](rhi::RenderPassEncoder& rp)
                                 { record(context, rp, viewProj, rect, viewContext); });
                });
            rhi::Buffer* buffer = slot.buffer;
            rendergraph::RenderGraph* g = &graph;
            graph.AddCopyPass(u8"pick.readback",
                              [ids, readback, rect, rowStride, buffer, g](rendergraph::PassBuilder& b)
                              {
                                  b.CopySrc(ids);
                                  b.CopyDst(readback);
                                  b.NeverCull();
                                  b.SetCopyExecute(
                                      [ids, rect, rowStride, buffer, g](rhi::CommandEncoder& enc)
                                      {
                                          rhi::Texture* src = g->GetTexture(ids);
                                          if (src == nullptr || buffer == nullptr)
                                          {
                                              return;
                                          }
                                          rhi::BufferTextureCopyRegion region;
                                          region.bytesPerRow = rowStride;
                                          region.rowsPerImage = rect.height;
                                          region.textureExtent =
                                              rhi::Extent3D{rect.width, rect.height, 1};
                                          enc.CopyTextureToBuffer(src, buffer, region);
                                      });
                              });
            slot.state = Slot::State::AwaitReadback;
            slot.submittedIndex = frameIndex;
            slot.sawOtherIndex = false;
            ++declared;
        }
        return declared;
    }
}
