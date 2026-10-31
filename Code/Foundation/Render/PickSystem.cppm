// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GPU picking: which entity is under a pixel (or inside a rect) of a view, answered by the GPU.
//
// A pick is an on-request pass: the requesting view's draw list is re-emitted through each
// renderer's ResolvePickIds (the depth-only caster path with an id-writing fragment) into a tiny
// RG32Uint target the size of the requested rect. The camera projection is cropped so the rect
// fills the whole target (a click renders a 1x1 target: vertex work only, almost no fragment
// cost). Its own depth keeps the nearest surface per texel; x is the entity index + 1 (0 for
// nothing) and y the entity generation, so a hit resolves to an exact EntityHandle. The target is
// copied into a GpuToCpu buffer by a graph copy pass and mapped once the device ring has left and
// returned to the submitting slot (the ThumbnailStage retire rule), so there is no WaitIdle and
// no stall.
//
// Keyed by the same opaque `viewportKey` a RenderScene call carries, so a request binds to the
// view of one viewport (split-screen, camera-preview insets and thumbnails never answer it).
module;
#include "Core/Prelude.h"
export module foundation.render:picking;
import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import :data;
import :resources;
using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace rhi = foundation::rhi;

export namespace foundation::render
{
    // A rect in view pixels (viewport-relative, y down, row 0 at the top), like the mouse position.
    struct PickRect
    {
        i32 x = 0;
        i32 y = 0;
        u32 width = 1;
        u32 height = 1;
    };

    // One decoded pick texel: the RenderData `entityId` tag split back out (EntityTag layout).
    struct PickHit
    {
        u32 entityIndex = 0;
        u32 generation = 0;
    };

    using PickRequestId = u32;
    inline constexpr PickRequestId kInvalidPickRequest = 0;

    struct PickResult
    {
        PickRequestId id = kInvalidPickRequest;
        Array<PickHit> hits;   // unique, first-seen order (row-major over the rect)
        bool rendered = false; // false: the view never rendered within the expiry window
    };

    inline constexpr rhi::TextureFormat kPickIdFormat = rhi::TextureFormat::RG32Uint;
    inline constexpr u32 kPickTexelBytes = 8;
    inline constexpr u32 kPickRowAlignment = 256; // texture-to-buffer copies: WebGPU and D3D12 row pitch

    // Clamp `rect` to the viewport; false when nothing is left (fully outside or empty).
    [[nodiscard]] bool ClampPickRect(PickRect& rect, u32 viewportWidth, u32 viewportHeight) noexcept;

    // A projection whose clip space is the pixel rect of a viewportWidth x viewportHeight view:
    // the rect's centre maps to NDC (0,0) and its edges to +-1, so rendering with it into a
    // rect.width x rect.height target reproduces exactly those pixels of the full view. Depth is
    // untouched (same near/far, same convention). Row-vector convention: clip = p * view * result.
    [[nodiscard]] Float4x4 CropProjectionToRect(const Float4x4& projection, const PickRect& rect,
                                                u32 viewportWidth, u32 viewportHeight) noexcept;

    [[nodiscard]] u32 PickRowStride(u32 width) noexcept;

    // Decode `height` rows of `width` RG32Uint texels (rows `rowStrideBytes` apart) into unique
    // hits, skipping background (x == 0).
    void DecodePickTexels(const u8* rows, u32 width, u32 height, u32 rowStrideBytes,
                          Array<PickHit>& out);

    // Records the pick draws into the pass: `viewProj` is the cropped world-to-clip, `rect` the
    // request (its size is the target size), `viewContext` what DeclarePasses was handed. A plain
    // function and context rather than a Function object: the frame holds no callable member.
    using PickRecordFn = void (*)(void* context, rhi::RenderPassEncoder& pass,
                                  const Float4x4& viewProj, const PickRect& rect,
                                  const void* viewContext);

    /// PickSystem::kExpireFrames, at namespace scope for another library to read (MSVC does not export
    /// a class's static constexpr member from a shared library).
    inline constexpr u32 kPickSystemExpireFrames = 32;

    class PickSystem
    {
    public:
        // A request that no view renders within this many frames completes as not rendered
        // (a hidden viewport must not leave the requester polling forever).
        static constexpr u32 kExpireFrames = kPickSystemExpireFrames;

        PickSystem(IAllocator& allocator, rhi::Device& device, u32 framesInFlight) noexcept;
        ~PickSystem();
        PickSystem(const PickSystem&) = delete;
        PickSystem& operator=(const PickSystem&) = delete;

        // Readback buffers retire through the queue (frames-in-flight safe); null means WaitIdle.
        void SetRetireQueue(GpuRetireQueue* retire) noexcept { m_retire = retire; }

        // Ask for the ids inside `rect` of the view that renders with `viewportKey`. The rect is
        // clamped when the view declares; a rect that clamps to nothing completes with no hits.
        [[nodiscard]] PickRequestId Request(const void* viewportKey, const PickRect& rect);

        // Poll a request: true once (the result moves out), then the id is forgotten.
        [[nodiscard]] bool TryTakeResult(PickRequestId id, PickResult& out);

        [[nodiscard]] bool IsPending(PickRequestId id) const noexcept;

        // Requests on `viewportKey` still waiting for a render (the passes the next frame declares).
        [[nodiscard]] u32 PendingCount(const void* viewportKey) const noexcept;

        // Drop every request on `viewportKey` (a viewport closing); in-flight readbacks still
        // retire, their results are discarded.
        void Cancel(const void* viewportKey);

        // ---- frame hooks (the RenderFrame drives these) ----

        // Retire readbacks whose submission provably completed (the ring left and returned to its
        // slot) and decode them; age requests toward expiry.
        void BeginFrame(u32 frameIndex);

        // Declare one pick pass and readback copy per request pending on `viewportKey`, for the
        // view whose camera is (`view`, `projection`) over a viewportWidth x viewportHeight
        // viewport. Returns the number declared.
        // `record(context, ...)` runs inside the pass; `viewContext` is handed back to it.
        u32 DeclarePasses(rendergraph::RenderGraph& graph, const void* viewportKey,
                          const Float4x4& view, const Float4x4& projection, u32 viewportWidth,
                          u32 viewportHeight, rhi::TextureFormat depthFormat, u32 frameIndex,
                          PickRecordFn record, void* context, const void* viewContext);

        // Total requests declared this frame across views (renderer ring sizing).
        [[nodiscard]] u32 PendingTotal() const noexcept;

    private:
        struct Slot
        {
            enum class State : u8
            {
                Free,
                Requested,     // waiting for its view to render
                AwaitReadback, // copy submitted on frame `submittedIndex`
                Ready,         // decoded; waiting for TryTakeResult
            };
            State state = State::Free;
            PickRequestId id = kInvalidPickRequest;
            const void* viewportKey = nullptr;
            PickRect rect{};
            rhi::Buffer* buffer = nullptr;
            u64 capacity = 0;
            u32 submittedIndex = 0;
            bool sawOtherIndex = false;
            bool rendered = false;
            u32 age = 0;
            Array<PickHit> hits;
        };

        [[nodiscard]] Slot* Find(PickRequestId id) noexcept;
        [[nodiscard]] const Slot* Find(PickRequestId id) const noexcept;
        [[nodiscard]] Slot& Acquire();
        [[nodiscard]] bool EnsureBuffer(Slot& slot, u64 bytes);
        void ReleaseBuffer(Slot& slot);

        IAllocator* m_allocator;
        rhi::Device* m_device;
        GpuRetireQueue* m_retire = nullptr;
        u32 m_framesInFlight;
        PickRequestId m_nextId = 1;
        Array<Slot> m_slots;
    };
}
