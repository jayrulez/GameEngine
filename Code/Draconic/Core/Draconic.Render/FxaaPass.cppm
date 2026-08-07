/// Draconic::Render - the `:fxaa` partition.
///
/// FXAA (Fast Approximate Anti-Aliasing), the TAA-OFF fallback AA. A single fullscreen LDR pass after
/// tonemap: perceptual-luma edge detect + directional edge search + sub-pixel blend (ported from the
/// SedulousEngine fxaa.frag quality variant). Only run when TAA is off - the two are never stacked
/// (locked decision) since TAA already resolves aliasing and FXAA on top would double-blur. Reads the
/// tonemapped LDR (a transient), writes the final target - both mapped to the view's sub-rect (so
/// split-screen views FXAA their own region), exactly like the tonemap.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:fxaa;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

    // Anti-aliases a tonemapped LDR transient into the final LDR target. Owns the fullscreen pipeline +
    // per-(view,frame) bind groups over the (transient) source view. Declared after tonemap, when TAA is off.
    class FxaaPass
    {
    public:
        FxaaPass(rhi::Device& device, shaders::ShaderSystem& shaders, u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight)
        {
        }
        ~FxaaPass() { Shutdown(); }
        FxaaPass(const FxaaPass&) = delete;
        FxaaPass& operator=(const FxaaPass&) = delete;

        Status Initialize()
        {

            rhi::BindGroupLayoutEntry texEntry =
                rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment);
            rhi::BindGroupLayoutEntry sampEntry =
                rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment);
            rhi::BindGroupLayoutEntry entries[] = {texEntry, sampEntry};
            rhi::BindGroupLayoutDesc ld{};
            ld.entries = Span<const rhi::BindGroupLayoutEntry>{entries, 2};
            if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }

            rhi::BindGroupLayout* layouts[] = {m_layout};
            rhi::PushConstantRange pc{};
            pc.stages = rhi::ShaderStage::Fragment;
            pc.offset = 0;
            pc.size = sizeof(f32) * 10;
            rhi::PipelineLayoutDesc pld{};
            pld.bindGroupLayouts = Span<rhi::BindGroupLayout* const>{layouts, 1};
            pld.pushConstantRanges = Span<const rhi::PushConstantRange>{&pc, 1};
            if (!m_device->CreatePipelineLayout(pld, m_pipelineLayout).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }

            rhi::SamplerDesc ss{};
            ss.minFilter = rhi::FilterMode::Linear;
            ss.magFilter = rhi::FilterMode::Linear;
            ss.addressU = rhi::AddressMode::ClampToEdge;
            ss.addressV = rhi::AddressMode::ClampToEdge;
            ss.addressW = rhi::AddressMode::ClampToEdge;
            ss.label = u8"fxaa.sampler";
            if (!m_device->CreateSampler(ss, m_sampler).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }
            return Status{};
        }

        // Declare the FXAA pass: read `src` (tonemapped LDR), write `ldr` (final), into the view's sub-rect.
        void DeclareFxaa(rendergraph::RenderGraph& graph, rendergraph::RGHandle src,
                         rendergraph::RGHandle ldr, bool clearColor, const rhi::ClearColor& clear,
                         rhi::TextureFormat ldrFormat, i32 vpX, i32 vpY, u32 vpW, u32 vpH,
                         u32 frameIndex, u32 viewIndex, Float2 texelSize, Float2 uvScale,
                         Float2 uvOffset, f32 subpixelQuality = 0.75f)
        {
            rhi::RenderPipeline* pipeline = EnsurePipeline(ldrFormat);
            if (pipeline == nullptr)
            {
                return;
            }
            const u32 slot =
                (viewIndex % kMaxViews) * m_framesInFlight + (frameIndex % m_framesInFlight);
            const f32 push[10] = {texelSize.x, texelSize.y,     uvScale.x, uvScale.y, uvOffset.x,
                                  uvOffset.y,  subpixelQuality, 0.166f,    0.0312f,   0.0f};

            const rhi::LoadOp load = clearColor ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
            graph.AddRenderPass(
                u8"fxaa",
                [this, &graph, src, ldr, load, clear, vpX, vpY, vpW, vpH, pipeline, slot,
                 push](rendergraph::PassBuilder& b)
                {
                    b.SetColorTarget(0, ldr, load, rhi::StoreOp::Store, clear);
                    b.ReadTexture(src);
                    b.SetViewport(vpX, vpY, vpW, vpH);
                    b.NeverCull();
                    b.SetExecute(
                        [this, &graph, src, pipeline, slot, push](rhi::RenderPassEncoder& rp)
                        {
                            rhi::BindGroup* bg = EnsureBindGroup(slot, graph.GetTextureView(src),
                                                                 graph.GetTextureGeneration(src));
                            if (bg == nullptr)
                            {
                                return;
                            }
                            rp.SetPipeline(pipeline);
                            rp.SetBindGroup(0, bg, Span<const u32>{});
                            rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(push), push);
                            rp.Draw(3, 1, 0, 0);
                        });
                });
        }

    private:
        static constexpr u32 kMaxViews = 8;
        static constexpr u32 kMaxFramesInFlight = 8;
        static constexpr u32 kMaxSlots = kMaxViews * kMaxFramesInFlight;

        rhi::RenderPipeline* EnsurePipeline(rhi::TextureFormat fmt)
        {
            const u64 shaderVersion = m_shaders->Version(u8"fxaa"); // hot reload rebuilds
            if (m_pipeline != nullptr && m_pipelineFormat == fmt &&
                m_pipelineShaderVersion == shaderVersion)
            {
                return m_pipeline;
            }
            rhi::ShaderModule* vs = m_shaders->GetVariant(u8"fxaa", shaders::ShaderStage::Vertex,
                                                          shaders::ShaderFlags::None);
            rhi::ShaderModule* ps = m_shaders->GetVariant(u8"fxaa", shaders::ShaderStage::Fragment,
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
            rhi::ColorTargetState color{};
            color.format = fmt;
            rhi::FragmentState frag{};
            frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
            frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
            rhi::RenderPipelineDesc pd{};
            pd.layout = m_pipelineLayout;
            pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
            pd.fragment = frag;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = rhi::CullMode::None;
            pd.label = u8"fxaa";
            if (!m_device->CreateRenderPipeline(pd, m_pipeline).IsOk())
            {
                m_pipeline = nullptr;
                return nullptr;
            }
            m_pipelineFormat = fmt;
            m_pipelineShaderVersion = shaderVersion;
            return m_pipeline;
        }

        rhi::BindGroup* EnsureBindGroup(u32 slot, rhi::TextureView* srcView, u64 generation)
        {
            if (slot >= kMaxSlots || srcView == nullptr)
            {
                return nullptr;
            }
            if (m_bindGroups[slot] != nullptr && m_bgViews[slot] == srcView &&
                m_bgGen[slot] == generation)
            {
                return m_bindGroups[slot];
            }
            if (m_bindGroups[slot] != nullptr)
            {
                m_device->DestroyBindGroup(m_bindGroups[slot]);
                m_bindGroups[slot] = nullptr;
            }
            rhi::BindGroupEntry entries[] = {
                rhi::BindGroupEntry::TextureEntry(srcView),
                rhi::BindGroupEntry::SamplerEntry(m_sampler),
            };
            rhi::BindGroupDesc bgd{};
            bgd.layout = m_layout;
            bgd.entries = Span<const rhi::BindGroupEntry>{entries, 2};
            if (!m_device->CreateBindGroup(bgd, m_bindGroups[slot]).IsOk())
            {
                m_bindGroups[slot] = nullptr;
                return nullptr;
            }
            m_bgViews[slot] = srcView;
            m_bgGen[slot] = generation;
            return m_bindGroups[slot];
        }

        void Shutdown()
        {
            for (u32 i = 0; i < kMaxSlots; ++i)
            {
                if (m_bindGroups[i] != nullptr)
                {
                    m_device->DestroyBindGroup(m_bindGroups[i]);
                    m_bindGroups[i] = nullptr;
                }
            }
            if (m_pipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
                m_pipeline = nullptr;
            }
            if (m_pipelineLayout != nullptr)
            {
                m_device->DestroyPipelineLayout(m_pipelineLayout);
                m_pipelineLayout = nullptr;
            }
            if (m_sampler != nullptr)
            {
                m_device->DestroySampler(m_sampler);
                m_sampler = nullptr;
            }
            if (m_layout != nullptr)
            {
                m_device->DestroyBindGroupLayout(m_layout);
                m_layout = nullptr;
            }
        }

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_framesInFlight = 2;
        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        rhi::TextureFormat m_pipelineFormat = rhi::TextureFormat::Undefined;
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::Sampler* m_sampler = nullptr;
        rhi::BindGroup* m_bindGroups[kMaxSlots] = {};
        rhi::TextureView* m_bgViews[kMaxSlots] = {};
        u64 m_bgGen[kMaxSlots] = {};
    };

} // namespace draconic::render
