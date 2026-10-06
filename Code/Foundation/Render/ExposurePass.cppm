// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:exposure` partition.
///
/// Auto-exposure: one 1x1 pass per view measures the HDR frame's geometric-mean luminance (a
/// fixed 16x16 sample grid over the view's sub-rect) and eases the PREVIOUS adapted value
/// toward it (exponential eye adaptation). The adapted 1x1 is a persistent per-view ping-pong
/// (the TAA-history pattern) imported into the graph, so the tonemap's read is a proper graph
/// edge. The tonemap turns it into a key/average multiplier clamped to the authored EV window.

module;
#include "Core/Prelude.h"

export module foundation.render:exposure;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{

    // Measures + adapts scene luminance into a persistent per-view 1x1 the tonemap samples.
    class ExposurePass
    {
    public:
        static constexpr u32 kMaxViews = 8;
        static constexpr u32 kMaxFramesInFlight = 8;
        static constexpr u32 kMaxSlots = kMaxViews * kMaxFramesInFlight;
        static constexpr rhi::TextureFormat kFormat = rhi::TextureFormat::R16Float;

        ExposurePass(rhi::Device& device, shaders::ShaderSystem& shaders,
                     u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight)
        {
        }
        ~ExposurePass() { Shutdown(); }
        ExposurePass(const ExposurePass&) = delete;
        ExposurePass& operator=(const ExposurePass&) = delete;

        Status Initialize()
        {
            rhi::BindGroupLayoutEntry entries[] = {
                rhi::BindGroupLayoutEntry::SampledTexture(0, rhi::ShaderStage::Fragment),
                rhi::BindGroupLayoutEntry::SampledTexture(1, rhi::ShaderStage::Fragment),
                rhi::BindGroupLayoutEntry::Sampler(0, rhi::ShaderStage::Fragment),
            };
            rhi::BindGroupLayoutDesc ld{};
            ld.entries = Span<const rhi::BindGroupLayoutEntry>{entries, 3};
            if (!m_device->CreateBindGroupLayout(ld, m_layout).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }
            rhi::BindGroupLayout* layouts[] = {m_layout};
            rhi::PushConstantRange pc{};
            pc.stages = rhi::ShaderStage::Fragment;
            pc.offset = 0;
            pc.size = sizeof(f32) * 8;
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
            ss.label = u8"exposure.sampler";
            if (!m_device->CreateSampler(ss, m_sampler).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }
            return Status{};
        }

        /// Declare the measure+adapt pass for one view. Returns the view's ADAPTED 1x1 as an
        /// imported handle (left in ShaderRead) for the tonemap to ReadTexture, plus the raw
        /// view to bind - or an invalid handle when the pass cannot run (missing shader/GPU).
        /// `sceneSerial` names the scene the view shows (ExtractedScene::SceneSerial): when it
        /// changes the history is dropped and the new scene's exposure snaps.
        struct Result
        {
            rendergraph::RGHandle handle{};
            rhi::TextureView* view = nullptr;
            u64 generation = 0; // bumps when the underlying texture changes (bind-group key)
        };
        [[nodiscard]] Result DeclareExposure(rendergraph::RenderGraph& graph,
                                             rendergraph::RGHandle hdr, u32 viewIndex,
                                             u32 frameIndex, Float2 uvScale, Float2 uvOffset,
                                             f32 deltaSeconds, f32 adaptSpeed,
                                             u64 sceneSerial = 0)
        {
            Result out;
            rhi::RenderPipeline* pipeline = EnsurePipeline();
            if (pipeline == nullptr)
            {
                return out;
            }
            ViewState& state = m_views[viewIndex % kMaxViews];
            if (!EnsureState(state))
            {
                return out;
            }
            // Another scene in this view (a level loaded, or the slot taken by another view's
            // scene): its exposure starts at its own level rather than easing from the old
            // scene's, which read as a dim and brighten at every level start.
            if (state.sceneSerial != sceneSerial)
            {
                state.valid = false;
                state.sceneSerial = sceneSerial;
            }
            state.snapped = !state.valid;
            const u32 cur = state.cur, prev = cur ^ 1u;
            const rendergraph::RGHandle prevH =
                graph.ImportTarget(u8"exposure.prev", state.tex[prev], state.view[prev],
                                   rhi::ResourceState::ShaderRead, state.state[prev]);
            state.state[prev] = rhi::ResourceState::ShaderRead;
            const rendergraph::RGHandle curH =
                graph.ImportTarget(u8"exposure.cur", state.tex[cur], state.view[cur],
                                   rhi::ResourceState::ShaderRead, state.state[cur]);
            state.state[cur] = rhi::ResourceState::ShaderRead;

            const f32 push[8] = {uvScale.x,
                                 uvScale.y,
                                 uvOffset.x,
                                 uvOffset.y,
                                 Max(deltaSeconds, 0.0f) * Max(adaptSpeed, 0.0f),
                                 state.valid ? 1.0f : 0.0f,
                                 0.0f,
                                 0.0f};
            rhi::TextureView* prevView = state.view[prev];
            // Per-(view, frame) slots (the FxaaPass scheme): prevView PING-PONGS every
            // frame, so a per-view-only slot mismatches every frame and EnsureBindGroup
            // then frees a set the previous frame's in-flight command buffer still
            // references (VUID-vkFreeDescriptorSets-00309 spam with auto-exposure on).
            // With frameIndex folded in, a slot is only rewritten framesInFlight frames
            // later - after its command buffer completed.
            const u32 slot =
                (viewIndex % kMaxViews) * m_framesInFlight + (frameIndex % m_framesInFlight);
            graph.AddRenderPass(
                u8"exposure.measure",
                [this, &graph, hdr, prevH, curH, prevView, pipeline, push,
                 slot](rendergraph::PassBuilder& b)
                {
                    b.SetColorTarget(0, curH, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                     rhi::ClearColor::Black());
                    b.ReadTexture(hdr);
                    b.ReadTexture(prevH);
                    b.SetViewport(0, 0, 1, 1);
                    b.NeverCull();
                    b.SetExecute(
                        [this, &graph, hdr, prevView, pipeline, push,
                         slot](rhi::RenderPassEncoder& rp)
                        {
                            rhi::BindGroup* bg =
                                EnsureBindGroup(slot, graph.GetTextureView(hdr), prevView,
                                                graph.GetTextureGeneration(hdr));
                            if (bg == nullptr)
                            {
                                return;
                            }
                            rp.SetPipeline(pipeline);
                            rp.SetBindGroup(0, bg, Span<const u32>{});
                            rp.SetPushConstants(rhi::ShaderStage::Fragment, 0, sizeof(push),
                                                push);
                            rp.Draw(3, 1, 0, 0);
                        });
                });

            state.valid = true;
            state.cur = prev; // ping-pong: next frame writes the other slot
            out.handle = curH;
            out.view = state.view[cur];
            out.generation = state.generation + cur;
            return out;
        }

        /// Whether `viewIndex`'s last declared pass took the measured value outright (its first
        /// frame, or the first frame of another scene) instead of easing from the last frame.
        [[nodiscard]] bool Snapped(u32 viewIndex) const noexcept
        {
            return m_views[viewIndex % kMaxViews].snapped;
        }

    private:
        struct ViewState
        {
            rhi::Texture* tex[2] = {};
            rhi::TextureView* view[2] = {};
            rhi::ResourceState state[2] = {rhi::ResourceState::Undefined,
                                           rhi::ResourceState::Undefined};
            u32 cur = 0;
            bool valid = false;
            bool snapped = false;  // the last declared pass took the measured value outright
            u64 sceneSerial = 0;   // the scene the history belongs to (ExtractedScene::SceneSerial)
            u64 generation = 0;
        };

        bool EnsureState(ViewState& state)
        {
            if (state.tex[0] != nullptr)
            {
                return true;
            }
            for (u32 i = 0; i < 2; ++i)
            {
                rhi::TextureDesc td{};
                td.format = kFormat;
                td.width = 1;
                td.height = 1;
                td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled;
                td.label = u8"exposure.adapted";
                if (!m_device->CreateTexture(td, state.tex[i]).IsOk())
                {
                    state.tex[i] = nullptr;
                    DestroyState(state);
                    return false;
                }
                rhi::TextureViewDesc vd{};
                vd.format = kFormat;
                vd.dimension = rhi::TextureViewDimension::Texture2D;
                if (!m_device->CreateTextureView(state.tex[i], vd, state.view[i]).IsOk())
                {
                    state.view[i] = nullptr;
                    DestroyState(state);
                    return false;
                }
                state.state[i] = rhi::ResourceState::Undefined;
            }
            state.cur = 0;
            state.valid = false;
            state.generation += 2;
            return true;
        }

        void DestroyState(ViewState& state)
        {
            for (u32 i = 0; i < 2; ++i)
            {
                if (state.view[i] != nullptr)
                {
                    m_device->DestroyTextureView(state.view[i]);
                    state.view[i] = nullptr;
                }
                if (state.tex[i] != nullptr)
                {
                    m_device->DestroyTexture(state.tex[i]);
                    state.tex[i] = nullptr;
                }
            }
            state.valid = false;
        }

        rhi::RenderPipeline* EnsurePipeline()
        {
            const u64 shaderVersion = m_shaders->Version(u8"exposure_measure");
            if (m_pipeline != nullptr && m_pipelineShaderVersion == shaderVersion)
            {
                return m_pipeline;
            }
            rhi::ShaderModule* vs = m_shaders->GetVariant(
                u8"exposure_measure", shaders::ShaderStage::Vertex, shaders::ShaderFlags::None);
            rhi::ShaderModule* ps = m_shaders->GetVariant(
                u8"exposure_measure", shaders::ShaderStage::Fragment, shaders::ShaderFlags::None);
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
            color.format = kFormat;
            rhi::FragmentState frag{};
            frag.shader = rhi::ProgrammableStage{ps, u8"main", rhi::ShaderStage::Fragment};
            frag.targets = Span<const rhi::ColorTargetState>{&color, 1};
            rhi::RenderPipelineDesc pd{};
            pd.layout = m_pipelineLayout;
            pd.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
            pd.fragment = frag;
            pd.primitive.topology = rhi::PrimitiveTopology::TriangleList;
            pd.primitive.cullMode = rhi::CullMode::None;
            pd.label = u8"exposure.measure";
            if (!m_device->CreateRenderPipeline(pd, m_pipeline).IsOk())
            {
                m_pipeline = nullptr;
                return nullptr;
            }
            m_pipelineShaderVersion = shaderVersion;
            return m_pipeline;
        }

        rhi::BindGroup* EnsureBindGroup(u32 slot, rhi::TextureView* hdrView,
                                        rhi::TextureView* prevView, u64 generation)
        {
            if (slot >= kMaxSlots || hdrView == nullptr || prevView == nullptr)
            {
                return nullptr;
            }
            Entry& entry = m_bindGroups[slot];
            if (entry.bg != nullptr && entry.hdr == hdrView && entry.prev == prevView &&
                entry.gen == generation)
            {
                return entry.bg;
            }
            if (entry.bg != nullptr)
            {
                m_device->DestroyBindGroup(entry.bg);
                entry.bg = nullptr;
            }
            rhi::BindGroupEntry entries[] = {
                rhi::BindGroupEntry::TextureEntry(hdrView),
                rhi::BindGroupEntry::TextureEntry(prevView),
                rhi::BindGroupEntry::SamplerEntry(m_sampler),
            };
            rhi::BindGroupDesc bd{};
            bd.layout = m_layout;
            bd.entries = Span<const rhi::BindGroupEntry>{entries, 3};
            if (!m_device->CreateBindGroup(bd, entry.bg).IsOk())
            {
                entry.bg = nullptr;
                return nullptr;
            }
            entry.hdr = hdrView;
            entry.prev = prevView;
            entry.gen = generation;
            return entry.bg;
        }

        void Shutdown()
        {
            for (Entry& entry : m_bindGroups)
            {
                if (entry.bg != nullptr)
                {
                    m_device->DestroyBindGroup(entry.bg);
                    entry.bg = nullptr;
                }
            }
            for (ViewState& state : m_views)
            {
                DestroyState(state);
            }
            if (m_pipeline != nullptr)
            {
                m_device->DestroyRenderPipeline(m_pipeline);
                m_pipeline = nullptr;
            }
            if (m_sampler != nullptr)
            {
                m_device->DestroySampler(m_sampler);
                m_sampler = nullptr;
            }
            if (m_pipelineLayout != nullptr)
            {
                m_device->DestroyPipelineLayout(m_pipelineLayout);
                m_pipelineLayout = nullptr;
            }
            if (m_layout != nullptr)
            {
                m_device->DestroyBindGroupLayout(m_layout);
                m_layout = nullptr;
            }
        }

        struct Entry
        {
            rhi::BindGroup* bg = nullptr;
            rhi::TextureView* hdr = nullptr;
            rhi::TextureView* prev = nullptr;
            u64 gen = 0;
        };

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_framesInFlight = 2;
        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        u64 m_pipelineShaderVersion = 0;
        rhi::Sampler* m_sampler = nullptr;
        ViewState m_views[kMaxViews];
        Entry m_bindGroups[kMaxSlots];
    };

} // namespace foundation::render
