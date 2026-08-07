/// Draconic::Imgui - the `:renderer` partition.
///
/// A Dear ImGui renderer built on Draconic's RHI (no stock imgui backend) - ported from the Sedulous
/// ImGui sample, which uses the same RHI shape. Owns the GPU pipeline + font atlas + per-frame dynamic
/// vertex/index/uniform buffers, and records ImDrawData into a render pass on a target each frame.
/// The font atlas upload is deferred to the first Render (we get a command encoder there, not at init).

module;
#include "Draconic.Foundation/Prelude.h"
#include "imgui.h"
#include <cstring>

export module draconic.imgui:renderer;

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.shaders.system;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::imgui
{

    // The ImGui VS/PS ship in the engine shader corpus (imgui.vs / imgui.ps) and are resolved via
    // ShaderSystem::GetVariant("imgui", ...) - cooked WGSL from the pack on web, dev-compiled from
    // Data/Shaders on desktop - so no inline HLSL / RegisterSource here.
    class ImguiRenderer
    {
    public:
        ImguiRenderer(rhi::Device& device, shaders::ShaderSystem& shaders,
                      u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_fif(framesInFlight < 1 ? 1 : (framesInFlight > kMaxFIF ? kMaxFIF : framesInFlight))
        {
        }
        ~ImguiRenderer() { Shutdown(); }
        ImguiRenderer(const ImguiRenderer&) = delete;
        ImguiRenderer& operator=(const ImguiRenderer&) = delete;

        Status Initialize()
        {
            // The "imgui" shaders come from the ShaderSystem (cooked pack or dev file provider) -
            // no RegisterSource; they are part of the engine corpus now.

            // set 0: projection UBO (b0, Vertex) + font texture (t0) + sampler (s0).
            rhi::BindGroupLayoutEntry projE =
                rhi::BindGroupLayoutEntry::UniformBuffer(0, rhi::ShaderStage::Vertex);
            rhi::BindGroupLayoutEntry texE =
                rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
            rhi::BindGroupLayoutEntry sampE =
                rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
            rhi::BindGroupLayoutEntry set0[] = {projE, texE, sampE};
            rhi::BindGroupLayoutDesc ld{};
            ld.entries = Span<const rhi::BindGroupLayoutEntry>{set0, 3};
            if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }
            rhi::BindGroupLayout* layouts[] = {m_layout};
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 1};
            if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }

            if (!CreateFontAtlas())
            {
                return Status{ErrorCode::Unknown};
            }

            // Per-frame-in-flight projection UBO + bind group (font/sampler shared; UBO per slot so an
            // in-flight frame's projection isn't overwritten).
            for (u32 i = 0; i < m_fif; ++i)
            {
                rhi::BufferDesc ud{};
                ud.size = sizeof(Float4x4);
                ud.usage = rhi::BufferUsage::Uniform;
                ud.memory = rhi::MemoryLocation::CpuToGpu;
                ud.label = u8"imgui.proj";
                if (!m_device->CreateBuffer(ud, m_frames[i].ubo).IsOk())
                {
                    return Status{ErrorCode::Unknown};
                }
                rhi::BindGroupEntry be[] = {
                    rhi::BindGroupEntry::BufferEntry(m_frames[i].ubo, 0, sizeof(Float4x4)),
                    rhi::BindGroupEntry::TextureEntry(m_fontView),
                    rhi::BindGroupEntry::SamplerEntry(m_sampler),
                };
                rhi::BindGroupDesc bgd{};
                bgd.layout = m_layout;
                bgd.entries = Span<const rhi::BindGroupEntry>{be, 3};
                if (!m_device->CreateBindGroup(bgd, m_frames[i].bindGroup).IsOk())
                {
                    return Status{ErrorCode::Unknown};
                }
            }
            return Status{};
        }

        // Record ImDrawData into a render pass on `target` (loaded, drawn on top of the scene). `enc` is the
        // frame's primary encoder; the font upload + buffer fills happen on it before the render pass opens.
        void Render(rhi::CommandEncoder& enc, rhi::TextureView* target,
                    rhi::TextureFormat targetFormat, u32 width, u32 height,
                    const ImDrawData* drawData, u32 frameIndex)
        {
            if (target == nullptr || drawData == nullptr)
            {
                return;
            }
            EnsureFontUploaded(enc);
            rhi::RenderPipeline* pipeline = EnsurePipeline(targetFormat);
            if (pipeline == nullptr)
            {
                return;
            }
            if (drawData->TotalVtxCount <= 0 || drawData->CmdListsCount <= 0)
            {
                return;
            }

            FrameSlot& slot = m_frames[frameIndex % m_fif];
            if (!UploadGeometry(slot, drawData))
            {
                return;
            }

            // Ortho off-center (0..W, 0..H, y-down) as a row-major matrix for row-vector mul.
            const f32 W = (drawData->DisplaySize.x > 0.0f) ? drawData->DisplaySize.x
                                                           : static_cast<f32>(width);
            const f32 H = (drawData->DisplaySize.y > 0.0f) ? drawData->DisplaySize.y
                                                           : static_cast<f32>(height);
            Float4x4 proj{};
            proj.m[0][0] = 2.0f / W;
            proj.m[1][1] = -2.0f / H;
            proj.m[2][2] = 1.0f;
            proj.m[3][3] = 1.0f;
            proj.m[3][0] = -1.0f;
            proj.m[3][1] = 1.0f;
            if (void* p = slot.ubo->Map())
            {
                MemCopy(p, &proj, sizeof(Float4x4));
                slot.ubo->Unmap();
            }

            rhi::ColorAttachment ca{};
            ca.view = target;
            ca.loadOp = rhi::LoadOp::Load;
            ca.storeOp = rhi::StoreOp::Store;
            rhi::RenderPassDesc rp{};
            rp.colorAttachments.Add(ca);
            rp.label = u8"imgui";
            rhi::RenderPassEncoder* pass = enc.BeginRenderPass(rp);
            if (pass == nullptr)
            {
                return;
            }
            pass->SetViewport(0.0f, 0.0f, static_cast<f32>(width), static_cast<f32>(height));
            pass->SetPipeline(pipeline);
            pass->SetVertexBuffer(0, slot.vtx, 0);
            pass->SetIndexBuffer(slot.idx, rhi::IndexFormat::UInt16, 0);

            const ImVec2 clipOff = drawData->DisplayPos;
            u32 globalVtx = 0, globalIdx = 0;
            for (i32 n = 0; n < drawData->CmdListsCount; ++n)
            {
                const ImDrawList* cmdList = drawData->CmdLists[n];
                for (i32 c = 0; c < cmdList->CmdBuffer.Size; ++c)
                {
                    const ImDrawCmd& cmd = cmdList->CmdBuffer[c];
                    if (cmd.ElemCount == 0 || cmd.UserCallback != nullptr)
                    {
                        continue;
                    }
                    // Clamp the clip rect to the actual render target: io.DisplaySize is one
                    // frame stale across a resize (set at NewFrame), and WebGPU validation drops
                    // the whole command buffer on a scissor outside the attachment.
                    i32 x0 = static_cast<i32>(cmd.ClipRect.x - clipOff.x);
                    i32 y0 = static_cast<i32>(cmd.ClipRect.y - clipOff.y);
                    i32 x1 = static_cast<i32>(cmd.ClipRect.z - clipOff.x);
                    i32 y1 = static_cast<i32>(cmd.ClipRect.w - clipOff.y);
                    x0 = x0 < 0 ? 0 : x0;
                    y0 = y0 < 0 ? 0 : y0;
                    x1 = x1 > static_cast<i32>(width) ? static_cast<i32>(width) : x1;
                    y1 = y1 > static_cast<i32>(height) ? static_cast<i32>(height) : y1;
                    if (x1 <= x0 || y1 <= y0)
                    {
                        continue;
                    }
                    pass->SetScissor(static_cast<u32>(x0), static_cast<u32>(y0),
                                     static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0));
                    pass->SetBindGroup(0, slot.bindGroup, Span<const u32>{});
                    pass->DrawIndexed(cmd.ElemCount, 1, cmd.IdxOffset + globalIdx,
                                      static_cast<i32>(cmd.VtxOffset + globalVtx), 0);
                }
                globalVtx += static_cast<u32>(cmdList->VtxBuffer.Size);
                globalIdx += static_cast<u32>(cmdList->IdxBuffer.Size);
            }
            pass->End();
        }

    private:
        static constexpr u32 kMaxFIF = 4;
        struct FrameSlot
        {
            rhi::Buffer* vtx = nullptr;
            u64 vtxCap = 0;
            rhi::Buffer* idx = nullptr;
            u64 idxCap = 0;
            rhi::Buffer* ubo = nullptr;
            rhi::BindGroup* bindGroup = nullptr;
        };

        bool CreateFontAtlas()
        {
            ImGuiIO& io = ImGui::GetIO();
            unsigned char* pixels = nullptr;
            int w = 0, h = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            if (pixels == nullptr || w <= 0 || h <= 0)
            {
                return false;
            }
            const u64 bytes = static_cast<u64>(w) * static_cast<u64>(h) * 4u;

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = static_cast<u32>(w);
            td.height = static_cast<u32>(h);
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = u8"imgui.font";
            if (!m_device->CreateTexture(td, m_fontTex).IsOk())
            {
                return false;
            }
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            vd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!m_device->CreateTextureView(m_fontTex, vd, m_fontView).IsOk())
            {
                return false;
            }

            rhi::BufferDesc sd{};
            sd.size = bytes;
            sd.usage = rhi::BufferUsage::CopySrc;
            sd.memory = rhi::MemoryLocation::CpuToGpu;
            sd.label = u8"imgui.fontStaging";
            if (!m_device->CreateBuffer(sd, m_fontStaging).IsOk())
            {
                return false;
            }
            if (void* p = m_fontStaging->Map())
            {
                MemCopy(p, pixels, bytes);
                m_fontStaging->Unmap();
            }
            m_fontW = static_cast<u32>(w);
            m_fontH = static_cast<u32>(h);

            rhi::SamplerDesc ss{};
            ss.minFilter = rhi::FilterMode::Linear;
            ss.magFilter = rhi::FilterMode::Linear;
            ss.mipmapFilter = rhi::MipmapFilterMode::Linear;
            ss.addressU = rhi::AddressMode::ClampToEdge;
            ss.addressV = rhi::AddressMode::ClampToEdge;
            ss.addressW = rhi::AddressMode::ClampToEdge;
            ss.label = u8"imgui.sampler";
            if (!m_device->CreateSampler(ss, m_sampler).IsOk())
            {
                return false;
            }

            io.Fonts->SetTexID(
                static_cast<ImTextureID>(1)); // single font texture; we always bind it
            return true;
        }

        void EnsureFontUploaded(rhi::CommandEncoder& enc)
        {
            if (m_fontUploaded || m_fontTex == nullptr || m_fontStaging == nullptr)
            {
                return;
            }
            enc.TransitionTexture(m_fontTex, rhi::ResourceState::Undefined,
                                  rhi::ResourceState::CopyDst);
            rhi::BufferTextureCopyRegion region{};
            region.bufferOffset = 0;
            region.bytesPerRow = m_fontW * 4u;
            region.rowsPerImage = m_fontH;
            region.textureExtent = rhi::Extent3D{m_fontW, m_fontH, 1};
            enc.CopyBufferToTexture(m_fontStaging, m_fontTex, region);
            enc.TransitionTexture(m_fontTex, rhi::ResourceState::CopyDst,
                                  rhi::ResourceState::ShaderRead);
#if DRACONIC_PLATFORM_WEB
            // A web startup submit can be dropped (the canvas texture expires if the frame
            // yields), which would lose this one-shot copy while the latch says done - ImGui
            // would render invisibly forever. Re-record the (tiny) copy for the first frames,
            // the same startup window the IBL env bake and probe captures use.
            m_fontUploaded = ++m_fontUploadFrames >= 20;
#else
            m_fontUploaded = true;
#endif
        }

        bool UploadGeometry(FrameSlot& slot, const ImDrawData* drawData)
        {
            const u64 vtxBytes = static_cast<u64>(drawData->TotalVtxCount) * sizeof(ImDrawVert);
            const u64 idxBytes = static_cast<u64>(drawData->TotalIdxCount) * sizeof(ImDrawIdx);
            if (vtxBytes == 0 || idxBytes == 0)
            {
                return false;
            }
            if (!EnsureBuffer(slot.vtx, slot.vtxCap, vtxBytes, rhi::BufferUsage::Vertex))
            {
                return false;
            }
            if (!EnsureBuffer(slot.idx, slot.idxCap, idxBytes, rhi::BufferUsage::Index))
            {
                return false;
            }

            auto* vdst = static_cast<u8*>(slot.vtx->Map());
            auto* idst = static_cast<u8*>(slot.idx->Map());
            if (vdst == nullptr || idst == nullptr)
            {
                if (vdst)
                    slot.vtx->Unmap();
                if (idst)
                    slot.idx->Unmap();
                return false;
            }
            u64 vo = 0, io_ = 0;
            for (i32 n = 0; n < drawData->CmdListsCount; ++n)
            {
                const ImDrawList* cl = drawData->CmdLists[n];
                const u64 vb = static_cast<u64>(cl->VtxBuffer.Size) * sizeof(ImDrawVert);
                const u64 ib = static_cast<u64>(cl->IdxBuffer.Size) * sizeof(ImDrawIdx);
                MemCopy(vdst + vo, cl->VtxBuffer.Data, vb);
                vo += vb;
                MemCopy(idst + io_, cl->IdxBuffer.Data, ib);
                io_ += ib;
            }
            slot.vtx->Unmap();
            slot.idx->Unmap();
            return true;
        }

        // Grow a per-slot dynamic buffer to at least `bytes` (1.5x headroom; rounded to 64KiB).
        bool EnsureBuffer(rhi::Buffer*& buf, u64& cap, u64 bytes, rhi::BufferUsage usage)
        {
            if (buf != nullptr && cap >= bytes)
            {
                return true;
            }
            if (buf != nullptr)
            {
                m_device->DestroyBuffer(buf);
                buf = nullptr;
            }
            u64 want = (bytes + bytes / 2u + 0xFFFFu) & ~static_cast<u64>(0xFFFFu);
            rhi::BufferDesc bd{};
            bd.size = want;
            bd.usage = usage;
            bd.memory = rhi::MemoryLocation::CpuToGpu;
            bd.label = u8"imgui.geom";
            if (!m_device->CreateBuffer(bd, buf).IsOk())
            {
                buf = nullptr;
                cap = 0;
                return false;
            }
            cap = want;
            return true;
        }

        rhi::RenderPipeline* EnsurePipeline(rhi::TextureFormat fmt)
        {
            if (m_pipeline != nullptr && m_pipelineFormat == fmt)
            {
                return m_pipeline;
            }
            rhi::ShaderModule* vs = m_shaders->GetVariant(u8"imgui", shaders::ShaderStage::Vertex,
                                                          shaders::ShaderFlags::None);
            rhi::ShaderModule* ps = m_shaders->GetVariant(u8"imgui", shaders::ShaderStage::Fragment,
                                                          shaders::ShaderFlags::None);
            if (vs == nullptr || ps == nullptr)
            {
                return nullptr;
            }
            if (m_pipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
                m_pipeline = nullptr;
            }

            // ImDrawVert = { ImVec2 pos; ImVec2 uv; ImU32 col } -> byte offsets 0 / 8 / 16, stride 20.
            rhi::VertexAttribute attrs[] = {
                {rhi::VertexFormat::Float32x2, 0, 0},
                {rhi::VertexFormat::Float32x2, 8, 1},
                {rhi::VertexFormat::Unorm8x4, 16, 2},
            };
            rhi::VertexBufferLayout vbl{};
            vbl.stride = sizeof(ImDrawVert);
            vbl.stepMode = rhi::VertexStepMode::Vertex;
            vbl.attributes = Span<const rhi::VertexAttribute>{attrs, 3};

            rhi::ColorTargetState color{};
            color.format = fmt;
            color.blend = rhi::BlendState::AlphaBlend();
            rhi::FragmentState frag{};
            frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
            frag.targets = Span<const rhi::ColorTargetState>{&color, 1};

            rhi::RenderPipelineDesc pd{};
            pd.layout = m_pipelineLayout;
            pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
            pd.vertex.buffers = Span<const rhi::VertexBufferLayout>{&vbl, 1};
            pd.fragment = frag;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = rhi::CullMode::None;
            pd.label = u8"imgui";
            if (!m_device->CreateRenderPipeline(pd, m_pipeline).IsOk())
            {
                m_pipeline = nullptr;
                return nullptr;
            }
            m_pipelineFormat = fmt;
            return m_pipeline;
        }

        void Shutdown()
        {
            for (u32 i = 0; i < kMaxFIF; ++i)
            {
                if (m_frames[i].bindGroup)
                {
                    m_device->DestroyBindGroup(m_frames[i].bindGroup);
                    m_frames[i].bindGroup = nullptr;
                }
                if (m_frames[i].ubo)
                {
                    m_device->DestroyBuffer(m_frames[i].ubo);
                    m_frames[i].ubo = nullptr;
                }
                if (m_frames[i].vtx)
                {
                    m_device->DestroyBuffer(m_frames[i].vtx);
                    m_frames[i].vtx = nullptr;
                }
                if (m_frames[i].idx)
                {
                    m_device->DestroyBuffer(m_frames[i].idx);
                    m_frames[i].idx = nullptr;
                }
            }
            if (m_pipeline)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
                m_pipeline = nullptr;
            }
            if (m_sampler)
            {
                m_device->DestroySampler(m_sampler);
                m_sampler = nullptr;
            }
            if (m_fontStaging)
            {
                m_device->DestroyBuffer(m_fontStaging);
                m_fontStaging = nullptr;
            }
            if (m_fontView)
            {
                m_device->DestroyTextureView(m_fontView);
                m_fontView = nullptr;
            }
            if (m_fontTex)
            {
                m_device->DestroyTexture(m_fontTex);
                m_fontTex = nullptr;
            }
            if (m_pipelineLayout)
            {
                m_device->DestroyPipelineLayout(m_pipelineLayout);
                m_pipelineLayout = nullptr;
            }
            if (m_layout)
            {
                m_device->DestroyBindGroupLayout(m_layout);
                m_layout = nullptr;
            }
        }

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_fif = 2;

        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        rhi::TextureFormat m_pipelineFormat = rhi::TextureFormat::Undefined;

        rhi::Texture* m_fontTex = nullptr;
        rhi::TextureView* m_fontView = nullptr;
        rhi::Buffer* m_fontStaging = nullptr;
        rhi::Sampler* m_sampler = nullptr;
        u32 m_fontW = 0, m_fontH = 0;
        bool m_fontUploaded = false;
#if DRACONIC_PLATFORM_WEB
        u32 m_fontUploadFrames = 0; // web startup re-record window (see EnsureFontUploaded)
#endif

        FrameSlot m_frames[kMaxFIF] = {};
    };

} // namespace draconic::imgui
