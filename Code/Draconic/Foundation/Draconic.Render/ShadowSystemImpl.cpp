/// Draconic::Render - the `:shadows` partition.
///
/// Shadow mapping (phase 5). 5.1 is the directional vertical slice: a single shadow map rendered
/// from the scene's directional shadow caster's point of view, sampled with PCF in the forward
/// shader. This partition OWNS the shadow depth texture(s) (one per frame-in-flight) and imports
/// them into the frame graph; RenderFrame (:pipeline, which has the renderer registry) declares the
/// depth pass that re-emits the casters and threads the binding into the forward pass. CSM cascades
/// (5.2) + the rebuilt atlas/scheduler (5.3/5.4, modeled on PlayCanvas) extend this.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.render;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import :data;  // ShadowCascades
import :views; // ViewCamera

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

namespace draconic::render
{
    rhi::TextureView* ShadowSystem::PrepareFrame(u32 frameIndex, u32 viewCount)
    {
        const u32 slot = frameIndex % m_framesInFlight;
        const u32 views =
            (viewCount < 1) ? 1u : (viewCount > kMaxShadowViews ? kMaxShadowViews : viewCount);
        const u32 layers = views * kCascadeCount;
        return EnsureTexture(slot, layers) ? m_sampleViews[slot] : nullptr;
    }

    rhi::TextureView* ShadowSystem::SampleView(u32 frameIndex) const noexcept
    {
        return m_sampleViews[frameIndex % m_framesInFlight];
    }

    rendergraph::RGHandle ShadowSystem::ImportTarget(rendergraph::RenderGraph& graph,
                                                     u32 frameIndex)
    {
        const u32 slot = frameIndex % m_framesInFlight;
        if (m_textures[slot] == nullptr)
        {
            return {};
        } // PrepareFrame creates it (sized for the views)
        const rendergraph::RGHandle h = graph.ImportTarget(
            u8"shadow.map", m_textures[slot], m_attachViews[slot], m_sampleViews[slot],
            rhi::ResourceState::DepthStencilRead, m_states[slot]);
        m_states[slot] = rhi::ResourceState::DepthStencilRead;
        return h;
    }

    rhi::TextureView* ShadowSystem::PrepareAtlas(u32 frameIndex)
    {
        const u32 slot = frameIndex % m_framesInFlight;
        return EnsureAtlas(slot) ? m_atlasSampleViews[slot] : nullptr;
    }

    rendergraph::RGHandle ShadowSystem::ImportAtlas(rendergraph::RenderGraph& graph, u32 frameIndex)
    {
        const u32 slot = frameIndex % m_framesInFlight;
        if (m_atlasTextures[slot] == nullptr)
        {
            return {};
        }
        const rendergraph::RGHandle h = graph.ImportTarget(
            u8"shadow.atlas", m_atlasTextures[slot], m_atlasAttachViews[slot],
            m_atlasSampleViews[slot], rhi::ResourceState::DepthStencilRead, m_atlasStates[slot]);
        m_atlasStates[slot] = rhi::ResourceState::DepthStencilRead;
        return h;
    }

    bool ShadowSystem::EnsureTexture(u32 slot, u32 layerCount)
    {
        if (m_textures[slot] != nullptr && m_layerCounts[slot] >= layerCount)
        {
            return true;
        }
        if (m_textures[slot] != nullptr)
        { // grow: replace the old array + its views. In-flight frames may still sample
          // them - RETIRE via the queue when wired (web-safe: no mid-frame WaitIdle
          // pumping the browser event loop), else drain.
            if (m_retire != nullptr)
            {
                m_retire->Retire(m_sampleViews[slot]);
                m_retire->Retire(m_attachViews[slot]);
                m_retire->Retire(m_textures[slot]);
                m_sampleViews[slot] = nullptr;
                m_attachViews[slot] = nullptr;
                m_textures[slot] = nullptr;
            }
            else
            {
                m_device->WaitIdle();
                if (m_sampleViews[slot] != nullptr)
                {
                    m_device->DestroyTextureView(m_sampleViews[slot]);
                    m_sampleViews[slot] = nullptr;
                }
                if (m_attachViews[slot] != nullptr)
                {
                    m_device->DestroyTextureView(m_attachViews[slot]);
                    m_attachViews[slot] = nullptr;
                }
                m_device->DestroyTexture(m_textures[slot]);
                m_textures[slot] = nullptr;
            }
        }
        rhi::TextureDesc td{};
        td.format = kShadowFormat;
        td.width = kShadowResolution;
        td.height = kShadowResolution;
        td.arrayLayerCount = layerCount;
        td.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
        td.label = u8"shadow.cascades";
        if (!m_device->CreateTexture(td, m_textures[slot]).IsOk())
        {
            m_textures[slot] = nullptr;
            return false;
        }

        // Whole-array views (the import's attachment fallback + the forward's Texture2DArray sample).
        rhi::TextureViewDesc av{};
        av.format = kShadowFormat;
        av.aspect = rhi::TextureAspect::DepthOnly;
        av.dimension = rhi::TextureViewDimension::Texture2DArray;
        av.arrayLayerCount = layerCount;
        if (!m_device->CreateTextureView(m_textures[slot], av, m_attachViews[slot]).IsOk())
        {
            return false;
        }
        rhi::TextureViewDesc sv{};
        sv.format = kShadowFormat;
        sv.aspect = rhi::TextureAspect::DepthOnly;
        sv.dimension = rhi::TextureViewDimension::Texture2DArray;
        sv.arrayLayerCount = layerCount;
        if (!m_device->CreateTextureView(m_textures[slot], sv, m_sampleViews[slot]).IsOk())
        {
            return false;
        }
        m_layerCounts[slot] = layerCount;
        m_states[slot] = rhi::ResourceState::Undefined;
        ++m_generation; // a new physical shadow texture exists -> invalidate consumer bind-group caches
        return true;
    }

    bool ShadowSystem::EnsureAtlas(u32 slot)
    {
        if (m_atlasTextures[slot] != nullptr)
        {
            return true;
        }
        rhi::TextureDesc td{};
        td.format = kShadowFormat;
        td.width = kAtlasResolution;
        td.height = kAtlasResolution;
        td.arrayLayerCount = kAtlasLayers;
        td.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
        td.label = u8"shadow.atlas";
        if (!m_device->CreateTexture(td, m_atlasTextures[slot]).IsOk())
        {
            m_atlasTextures[slot] = nullptr;
            return false;
        }
        rhi::TextureViewDesc av{};
        av.format = kShadowFormat;
        av.aspect = rhi::TextureAspect::DepthOnly;
        av.dimension = rhi::TextureViewDimension::Texture2DArray;
        av.arrayLayerCount = kAtlasLayers;
        if (!m_device->CreateTextureView(m_atlasTextures[slot], av, m_atlasAttachViews[slot])
                 .IsOk())
        {
            return false;
        }
        rhi::TextureViewDesc sv{};
        sv.format = kShadowFormat;
        sv.aspect = rhi::TextureAspect::DepthOnly;
        sv.dimension = rhi::TextureViewDimension::Texture2DArray;
        sv.arrayLayerCount = kAtlasLayers;
        if (!m_device->CreateTextureView(m_atlasTextures[slot], sv, m_atlasSampleViews[slot])
                 .IsOk())
        {
            return false;
        }
        m_atlasStates[slot] = rhi::ResourceState::Undefined;
        ++m_generation;
        return true;
    }

    void ShadowSystem::Shutdown()
    {
        for (u32 i = 0; i < kMaxFramesInFlight; ++i)
        {
            if (m_sampleViews[i] != nullptr)
            {
                m_device->DestroyTextureView(m_sampleViews[i]);
                m_sampleViews[i] = nullptr;
            }
            if (m_attachViews[i] != nullptr)
            {
                m_device->DestroyTextureView(m_attachViews[i]);
                m_attachViews[i] = nullptr;
            }
            if (m_textures[i] != nullptr)
            {
                m_device->DestroyTexture(m_textures[i]);
                m_textures[i] = nullptr;
            }
            if (m_atlasSampleViews[i] != nullptr)
            {
                m_device->DestroyTextureView(m_atlasSampleViews[i]);
                m_atlasSampleViews[i] = nullptr;
            }
            if (m_atlasAttachViews[i] != nullptr)
            {
                m_device->DestroyTextureView(m_atlasAttachViews[i]);
                m_atlasAttachViews[i] = nullptr;
            }
            if (m_atlasTextures[i] != nullptr)
            {
                m_device->DestroyTexture(m_atlasTextures[i]);
                m_atlasTextures[i] = nullptr;
            }
        }
    }
}
