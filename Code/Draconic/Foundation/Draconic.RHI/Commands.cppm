/// Abstract command recording interfaces: CommandPool, CommandEncoder,
/// RenderPassEncoder, ComputePassEncoder, TransferBatch.

export module draconic.rhi:commands;

import draconic.foundation;
import :forward;
import :enums;
import :types;
import :descriptors;
import :resources;

using namespace draconic::foundation;

export namespace draconic::rhi
{

    // ---- Render Command Encoder (shared draw-recording surface) ----

    /// The draw-recording commands common to a render pass and a render bundle. A `Renderer` that
    /// takes a `RenderCommandEncoder*` records identically whether it targets a live pass (inline)
    /// or an off-thread `RenderBundleEncoder` - which is what makes parallel command recording
    /// fall out (split a draw list into N bundles recorded on N threads, then ExecuteBundles).
    /// This is exactly the subset valid inside a WebGPU render bundle: no pass-level dynamic state
    /// (viewport / scissor / blend constant / stencil ref are inherited from the pass), no queries.
    class RenderCommandEncoder
    {
    public:
        virtual ~RenderCommandEncoder() = default;

        /// Bind a graphics (rasterization) pipeline.
        virtual void SetPipeline(RenderPipeline* pipeline) = 0;
        /// Bind a resource group at the given index.
        virtual void SetBindGroup(u32 index, BindGroup* group,
                                  Span<const u32> dynamicOffsets = {}) = 0;
        /// Upload push constant data.
        virtual void SetPushConstants(ShaderStage stages, u32 offset, u32 size,
                                      const void* data) = 0;

        /// Bind a vertex buffer to a slot.
        virtual void SetVertexBuffer(u32 slot, Buffer* buffer, u64 offset = 0) = 0;
        /// Bind an index buffer.
        virtual void SetIndexBuffer(Buffer* buffer, IndexFormat format, u64 offset = 0) = 0;

        /// Issue a non-indexed draw call.
        virtual void Draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0,
                          u32 firstInstance = 0) = 0;
        /// Issue an indexed draw call.
        virtual void DrawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0,
                                 i32 baseVertex = 0, u32 firstInstance = 0) = 0;
        /// Issue an indirect draw call.
        virtual void DrawIndirect(Buffer* buffer, u64 offset, u32 drawCount = 1,
                                  u32 stride = 0) = 0;
        /// Issue an indexed indirect draw call.
        virtual void DrawIndexedIndirect(Buffer* buffer, u64 offset, u32 drawCount = 1,
                                         u32 stride = 0) = 0;
    };

    // ---- Render Pass Encoder ----

    /// Records draw commands within a render pass: the shared recording surface plus the pass-level
    /// dynamic state, queries, bundle execution, and pass control.
    class RenderPassEncoder : public RenderCommandEncoder
    {
    public:
        /// Cross-query for the mesh-shader extension (-fno-rtti replacement for a
        /// sideways dynamic_cast). Encoders that support it return `this`.
        [[nodiscard]] virtual MeshShaderPassExt* AsMeshShaderExt() noexcept { return nullptr; }

        /// Set the viewport rectangle and depth range.
        virtual void SetViewport(f32 x, f32 y, f32 w, f32 h, f32 minDepth = 0.0f,
                                 f32 maxDepth = 1.0f) = 0;
        /// Set the scissor rectangle.
        virtual void SetScissor(i32 x, i32 y, u32 w, u32 h) = 0;
        /// Set the blend constant color.
        virtual void SetBlendConstant(f32 r, f32 g, f32 b, f32 a) = 0;
        /// Set the stencil reference value.
        virtual void SetStencilReference(u32 reference) = 0;

        /// Execute pre-recorded render bundles in order (replays their draws into this pass). The
        /// pass must have been begun with RenderPassContents::SecondaryCommandBuffers.
        virtual void ExecuteBundles(Span<RenderBundle* const> bundles) = 0;

        /// Write a timestamp query.
        virtual void WriteTimestamp(QuerySet* querySet, u32 index) = 0;
        /// Begin an occlusion query.
        virtual void BeginOcclusionQuery(QuerySet* querySet, u32 index) = 0;
        /// End an occlusion query.
        virtual void EndOcclusionQuery(QuerySet* querySet, u32 index) = 0;

        /// End the render pass.
        virtual void End() = 0;
    };

    // ---- Render Bundle ----

    /// An immutable, pre-recorded sequence of draw commands, replayable into any compatible render
    /// pass (matching attachment formats) via RenderPassEncoder::ExecuteBundles. Valid until its
    /// owning command pool is reset. Recorded off the main thread for parallel command recording.
    class RenderBundle
    {
    public:
        virtual ~RenderBundle() = default;
    };

    /// Records draws into a render bundle (the shared recording surface only - no pass-level state).
    class RenderBundleEncoder : public RenderCommandEncoder
    {
    public:
        /// Finish recording and return the immutable bundle (owned by the command pool).
        [[nodiscard]] virtual RenderBundle* Finish() = 0;
    };

    // ---- Compute Pass Encoder ----

    /// Records compute dispatch commands within a compute pass.
    class ComputePassEncoder
    {
    public:
        virtual ~ComputePassEncoder() = default;

        virtual void SetPipeline(ComputePipeline* pipeline) = 0;
        virtual void SetBindGroup(u32 index, BindGroup* group,
                                  Span<const u32> dynamicOffsets = {}) = 0;
        virtual void SetPushConstants(ShaderStage stages, u32 offset, u32 size,
                                      const void* data) = 0;

        /// Dispatch compute work groups.
        virtual void Dispatch(u32 x, u32 y = 1, u32 z = 1) = 0;
        /// Dispatch compute work groups via an indirect buffer.
        virtual void DispatchIndirect(Buffer* buffer, u64 offset) = 0;

        /// Insert a compute-to-compute memory barrier.
        virtual void ComputeBarrier() = 0;

        virtual void WriteTimestamp(QuerySet* querySet, u32 index) = 0;
        virtual void End() = 0;
    };

    // ---- Command Encoder ----

    /// Records GPU commands: render/compute passes, barriers, copies, queries.
    class CommandEncoder
    {
    public:
        virtual ~CommandEncoder() = default;

        /// Cross-query for the ray-tracing extension (-fno-rtti replacement for a
        /// sideways dynamic_cast). Encoders that support it return `this`.
        [[nodiscard]] virtual RayTracingEncoderExt* AsRayTracingExt() noexcept { return nullptr; }

        /// Begin a render pass. Returns the encoder for recording draw commands.
        [[nodiscard]] virtual RenderPassEncoder* BeginRenderPass(const RenderPassDesc& desc) = 0;
        /// Begin a compute pass.
        [[nodiscard]] virtual ComputePassEncoder* BeginComputePass(StringView label = {}) = 0;
        /// Begin recording a render bundle (a reusable, off-thread-recordable draw sequence
        /// replayable into passes matching `desc`'s attachment signature). Convenience for
        /// CommandPool::CreateRenderBundleEncoder on this encoder's pool: the bundle is owned
        /// by the pool and lives until the pool's next Reset(). Returns null if the backend
        /// does not support bundles.
        [[nodiscard]] virtual RenderBundleEncoder*
        CreateRenderBundleEncoder(const RenderBundleDesc& desc) = 0;

        /// Insert resource barriers.
        virtual void Barrier(const BarrierGroup& group) = 0;

        /// Convenience: transition a single texture between resource states.
        void TransitionTexture(Texture* tex, ResourceState oldState, ResourceState newState)
        {
            TextureBarrier tb{};
            tb.texture = tex;
            tb.oldState = oldState;
            tb.newState = newState;
            BarrierGroup g{};
            g.textureBarriers = Span<const TextureBarrier>(&tb, 1);
            Barrier(g);
        }

        /// Convenience: transition a single buffer between resource states.
        void TransitionBuffer(Buffer* buf, ResourceState oldState, ResourceState newState)
        {
            BufferBarrier bb{};
            bb.buffer = buf;
            bb.oldState = oldState;
            bb.newState = newState;
            BarrierGroup g{};
            g.bufferBarriers = Span<const BufferBarrier>(&bb, 1);
            Barrier(g);
        }

        /// Copy operations.
        virtual void CopyBufferToBuffer(Buffer* src, u64 srcOffset, Buffer* dst, u64 dstOffset,
                                        u64 size) = 0;
        virtual void CopyBufferToTexture(Buffer* src, Texture* dst,
                                         const BufferTextureCopyRegion& region) = 0;
        virtual void CopyTextureToBuffer(Texture* src, Buffer* dst,
                                         const BufferTextureCopyRegion& region) = 0;
        virtual void CopyTextureToTexture(Texture* src, Texture* dst,
                                          const TextureCopyRegion& region) = 0;

        /// Blit (scaled copy) from one texture to another.
        virtual void Blit(Texture* src, Texture* dst) = 0;
        /// Generate mipmaps for a texture.
        virtual void GenerateMipmaps(Texture* texture) = 0;
        /// Resolve a multisampled texture to a single-sampled texture.
        virtual void ResolveTexture(Texture* src, Texture* dst) = 0;

        /// Query operations.
        virtual void ResetQuerySet(QuerySet* querySet, u32 first, u32 count) = 0;
        virtual void WriteTimestamp(QuerySet* querySet, u32 index) = 0;
        virtual void ResolveQuerySet(QuerySet* querySet, u32 first, u32 count, Buffer* dst,
                                     u64 dstOffset) = 0;

        /// Debug labels.
        virtual void BeginDebugLabel(StringView label, f32 r = 0, f32 g = 0, f32 b = 0,
                                     f32 a = 1) = 0;
        virtual void EndDebugLabel() = 0;
        virtual void InsertDebugLabel(StringView label, f32 r = 0, f32 g = 0, f32 b = 0,
                                      f32 a = 1) = 0;

        /// Finish recording and return an immutable command buffer.
        [[nodiscard]] virtual CommandBuffer* Finish() = 0;
    };

    // ---- Command Pool ----

    /// Manages command buffer memory for a single queue type.
    /// One pool per thread per queue type.
    class CommandPool
    {
    public:
        virtual ~CommandPool() = default;

        /// Create a new command encoder for recording. Every encoder MUST call
        /// Finish() before this pool's next Reset() — DX12 cannot reset a
        /// command allocator while one of its command lists is still recording.
        virtual Status CreateEncoder(CommandEncoder*& out) = 0;
        /// Destroy a command encoder.
        virtual void DestroyEncoder(CommandEncoder*& encoder) = 0;
        /// Reset all command buffers allocated from this pool, and free the
        /// render bundles it produced this cycle. Call only after the GPU has
        /// finished the pool's last submission (fence-guarded).
        virtual void Reset() = 0;

        /// Begin recording a render bundle from this pool — no open command
        /// encoder required, so per-thread bundle pools need only a pool.
        /// The returned encoder and its Finish()ed bundle are OWNED BY THE POOL
        /// and stay valid until the pool's next Reset(). Returns null if the
        /// backend does not support bundles.
        [[nodiscard]] virtual RenderBundleEncoder*
        CreateRenderBundleEncoder(const RenderBundleDesc& desc) = 0;
    };

    // ---- Transfer Batch ----

    /// Batches staging upload operations (CPU→GPU buffer/texture writes).
    class TransferBatch
    {
    public:
        virtual ~TransferBatch() = default;

        /// Stage a buffer write.
        virtual void WriteBuffer(Buffer* dst, u64 dstOffset, Span<const u8> data) = 0;
        /// Stage a texture write.
        virtual void WriteTexture(Texture* dst, Span<const u8> data,
                                  const TextureDataLayout& layout, Extent3D extent,
                                  u32 mipLevel = 0, u32 arrayLayer = 0) = 0;

        /// Submit all staged writes synchronously.
        virtual Status Submit() = 0;
        /// Submit all staged writes, signaling a fence on completion.
        virtual Status SubmitAsync(Fence* fence, u64 signalValue) = 0;

        /// Reset the batch for reuse.
        virtual void Reset() = 0;
        /// Destroy the batch.
        virtual void Destroy() = 0;
    };

} // namespace draconic::rhi
