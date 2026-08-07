/// Draconic::PipelineCache - the `draconic.materials.pipelinecache` module.
///
/// The render-side PSO cache: the one piece of the shader/material stack that lives
/// outside the resource system (the "lone exception" from the hot-reload design). It
/// maps a PipelineConfig (× render-target signature) to a compiled RHI RenderPipeline,
/// pulling shader variants from the ShaderSystem and mapping the material's render
/// state to RHI pipeline state.
///
/// Hot reload is handled by VERSION POLLING at point of use: each cached entry records
/// the ShaderSystem version of its shader at build time; GetPipeline compares against
/// the current version and lazily rebuilds when a shader was invalidated (reloaded).
/// This costs one HashMap lookup + integer compare per draw - the same as a dirty-flag
/// check - with no listener bookkeeping. Superseded pipelines are retired to a
/// graveyard and freed by ReleaseRetired() once the GPU is done with them.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.materials.pipelinecache;

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import draconic.shaders.system;
import draconic.materials;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::materials
{

    class PipelineStateCache
    {
    public:
        PipelineStateCache(shaders::ShaderSystem& shaderSystem, rhi::Device& device) noexcept
            : m_shaders(&shaderSystem), m_device(&device)
        {
        }

        ~PipelineStateCache()
        {
            Clear();
            ReleaseRetired();
        }

        PipelineStateCache(const PipelineStateCache&) = delete;
        PipelineStateCache& operator=(const PipelineStateCache&) = delete;

        // Returns a RenderPipeline for (config, layout, colorOverride), building it on the
        // first request and rebuilding it when the shader has since been reloaded. `layout`
        // is the assembled pipeline layout (frame/object/material bind-group layouts); the
        // caller owns it. Returns null on build failure.
        [[nodiscard]] rhi::RenderPipeline*
        GetPipeline(const PipelineConfig& config, rhi::PipelineLayout* layout,
                    rhi::TextureFormat colorOverride = rhi::TextureFormat::Undefined)
        {
            const u64 key = KeyOf(config, layout, colorOverride);
            const u64 version = m_shaders->Version(config.shaderName);

            if (Entry* e = m_entries.Find(key))
            {
                if (e->builtVersion == version && e->pipeline != nullptr)
                {
                    return e->pipeline;
                }
                // shader was reloaded since this PSO was built: retire the stale one + rebuild.
                if (e->pipeline != nullptr)
                {
                    m_retired.PushBack(e->pipeline);
                }
                e->pipeline = Build(config, layout, colorOverride);
                e->builtVersion = version;
                return e->pipeline;
            }

            Entry entry{};
            entry.pipeline = Build(config, layout, colorOverride);
            entry.builtVersion = version;
            rhi::RenderPipeline* result = entry.pipeline;
            m_entries.InsertOrAssign(key, entry);
            return result;
        }

        [[nodiscard]] usize Size() const noexcept { return m_entries.Size(); }
        [[nodiscard]] usize RetiredCount() const noexcept { return m_retired.Size(); }

        // Frees pipelines superseded by a reload. Call once the frames that may still
        // reference them have completed (the GraphicsDevice frame ring is the gate).
        void ReleaseRetired()
        {
            for (rhi::RenderPipeline* p : m_retired)
            {
                if (p != nullptr)
                {
                    m_device->DestroyRenderPipeline(p);
                }
            }
            m_retired.Clear();
        }

        // Destroys all live pipelines (retires nothing - call at shutdown).
        void Clear()
        {
            for (auto& e : m_entries)
            {
                if (e.value.pipeline != nullptr)
                {
                    m_device->DestroyRenderPipeline(e.value.pipeline);
                }
            }
            m_entries.Clear();
        }

    private:
        struct Entry
        {
            rhi::RenderPipeline* pipeline = nullptr;
            u64 builtVersion = 0;
        };

        static u64 KeyOf(const PipelineConfig& config, rhi::PipelineLayout* layout,
                         rhi::TextureFormat colorOverride)
        {
            u64 h = config.HashCode();
            h = h * 31u + reinterpret_cast<u64>(layout);
            h = h * 31u + static_cast<u64>(colorOverride);
            return h;
        }

        rhi::RenderPipeline* Build(const PipelineConfig& config, rhi::PipelineLayout* layout,
                                   rhi::TextureFormat colorOverride)
        {
            rhi::ShaderModule* vs = m_shaders->GetVariant(
                config.shaderName, shaders::ShaderStage::Vertex, config.shaderFlags);
            if (vs == nullptr)
            {
                return nullptr;
            }

            rhi::RenderPipelineDesc desc{};
            desc.layout = layout;
            desc.label = config.shaderName;

            // --- vertex ---
            // Up to three buffers, in slot order matching the renderer's draw bindings: mesh stream
            // (slot 0); the skinning stream (slot 1, joints loc 6 + weights loc 7) when skinned; the
            // instance-stepped DataOffsets stream (loc 5) when instanced. Skinned draws are always
            // instanced -> [mesh, skin, offsets]; non-skinned instanced -> [mesh, offsets].
            rhi::VertexBufferLayout buffers[3] = {
                VertexLayoutHelper::BufferLayout(config.vertexLayout), {}, {}};
            u32 bufferCount = (config.vertexLayout != VertexLayoutType::None) ? 1u : 0u;
            if (config.vertexLayout == VertexLayoutType::SkinnedMesh)
            {
                buffers[bufferCount++] = VertexLayoutHelper::SkinningStreamBufferLayout();
            }
            if (config.instanced)
            {
                buffers[bufferCount++] = VertexLayoutHelper::InstanceOffsetsBufferLayout();
            }
            desc.vertex.shader = rhi::ProgrammableStage{vs, u8"main", rhi::ShaderStage::Vertex};
            if (bufferCount > 0)
            {
                desc.vertex.buffers = Span<const rhi::VertexBufferLayout>{buffers, bufferCount};
            }

            // --- fragment (omitted for depth-only passes) ---
            // Up to colorTargetCount targets (MRT): target 0 is the shaded color (blended per blendMode +
            // format from colorOverride when set, e.g. the per-view HDR/LDR format); targets 1+ are the
            // G-buffer aux outputs (view-normal, motion vector) - no blend, formats from config.colorFormats.
            rhi::ColorTargetState colorTargets[rhi::MaxColorAttachments] = {};
            if (!config.depthOnly)
            {
                rhi::ShaderModule* fs = m_shaders->GetVariant(
                    config.shaderName, shaders::ShaderStage::Fragment, config.shaderFlags);
                if (fs == nullptr)
                {
                    return nullptr;
                }
                // Honor colorTargetCount exactly - 0 means a fragment that writes no color (e.g. the masked
                // shadow pass: alpha-test discard + depth only). No fallback to 1.
                const u32 count = Min<u32>(config.colorTargetCount, rhi::MaxColorAttachments);
                for (u32 i = 0; i < count; ++i)
                {
                    colorTargets[i].format =
                        (i == 0 && colorOverride != rhi::TextureFormat::Undefined)
                            ? colorOverride
                            : config.colorFormats[i];
                    colorTargets[i].blend =
                        (i == 0) ? BlendFor(config.blendMode) : Optional<rhi::BlendState>{};
                    // Aux G-buffer targets (1+) only write when writeAuxTargets is set - transparent draws
                    // bind them (to match the pass) but leave the opaque normal/velocity underneath intact.
                    colorTargets[i].writeMask = (i == 0 || config.writeAuxTargets)
                                                    ? config.colorWriteMask
                                                    : rhi::ColorWriteMask::None;
                }
                rhi::FragmentState frag{};
                frag.shader = rhi::ProgrammableStage{fs, u8"main", rhi::ShaderStage::Fragment};
                frag.targets = Span<const rhi::ColorTargetState>{colorTargets, count};
                desc.fragment = frag;
            }

            // --- primitive ---
            desc.primitive.topology = config.topology;
            desc.primitive.frontFace = config.frontFace;
            desc.primitive.cullMode = CullFor(config.cullMode);
            desc.primitive.fillMode = config.fillMode;

            // --- depth/stencil ---
            if (config.depthMode != DepthMode::Disabled)
            {
                rhi::DepthStencilState ds{};
                ds.format = config.depthFormat;
                ds.depthTestEnabled = config.depthMode == DepthMode::ReadWrite ||
                                      config.depthMode == DepthMode::ReadOnly;
                ds.depthWriteEnabled = config.depthMode == DepthMode::ReadWrite ||
                                       config.depthMode == DepthMode::WriteOnly;
                ds.depthCompare = config.depthCompare;
                ds.depthBias = config.depthBias;
                ds.depthBiasSlopeScale = config.depthBiasSlopeScale;
                desc.depthStencil = ds;
            }

            // --- multisample ---
            desc.multisample.count = config.sampleCount;

            rhi::RenderPipeline* pipeline = nullptr;
            if (!m_device->CreateRenderPipeline(desc, pipeline).IsOk())
            {
                return nullptr;
            }
            return pipeline;
        }

        static Optional<rhi::BlendState> BlendFor(BlendMode mode)
        {
            switch (mode)
            {
            case BlendMode::Opaque:
            case BlendMode::Masked:
                return {}; // no blending
            case BlendMode::AlphaBlend:
                return rhi::BlendState::AlphaBlend();
            case BlendMode::Additive:
                return rhi::BlendState{
                    {rhi::BlendFactor::One, rhi::BlendFactor::One, rhi::BlendOperation::Add},
                    {rhi::BlendFactor::One, rhi::BlendFactor::One, rhi::BlendOperation::Add}};
            case BlendMode::Multiply:
                return rhi::BlendState{
                    {rhi::BlendFactor::Dst, rhi::BlendFactor::Zero, rhi::BlendOperation::Add},
                    {rhi::BlendFactor::Dst, rhi::BlendFactor::Zero, rhi::BlendOperation::Add}};
            case BlendMode::PremultipliedAlpha:
                return rhi::BlendState{{rhi::BlendFactor::One, rhi::BlendFactor::OneMinusSrcAlpha,
                                        rhi::BlendOperation::Add},
                                       {rhi::BlendFactor::One, rhi::BlendFactor::OneMinusSrcAlpha,
                                        rhi::BlendOperation::Add}};
            }
            return {};
        }

        static rhi::CullMode CullFor(CullModeConfig c)
        {
            switch (c)
            {
            case CullModeConfig::None:
                return rhi::CullMode::None;
            case CullModeConfig::Back:
                return rhi::CullMode::Back;
            case CullModeConfig::Front:
                return rhi::CullMode::Front;
            }
            return rhi::CullMode::None;
        }

        shaders::ShaderSystem* m_shaders; // borrowed
        rhi::Device* m_device;            // borrowed
        HashMap<u64, Entry> m_entries;
        Array<rhi::RenderPipeline*> m_retired; // superseded by reload, awaiting GPU-safe free
    };

} // namespace draconic::materials
