// Draconic::RenderGraph - :resource partition
//
// A resource managed by the graph (texture or buffer): its descriptor, the
// allocated GPU object, reference/lifetime tracking computed during compile, and
// barrier state. Ported from Sedulous.RenderGraph (RenderGraphResource.bf).
// Fields are public data the graph orchestrator manipulates directly.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rendergraph:resource;

import draconic.foundation;
import draconic.rhi;
import :types;
import :descriptors;
import :persistent_resource;

using namespace draconic::foundation;

export namespace draconic::rendergraph
{
    namespace rhi = draconic::rhi;

    class RenderGraphResource
    {
    public:
        RenderGraphResource(StringView resourceName, RGResourceType type, RGResourceLifetime life)
            : name(resourceName), resourceType(type), lifetime(life)
        {
        }

        // Allocate GPU resources for a transient texture.
        [[nodiscard]] Status AllocateTexture(rhi::Device& device)
        {
            rhi::TextureDesc rhiDesc = textureDesc.ToTextureDesc(name.AsView());
            rhiDesc.usage = rhiDesc.usage | rhi::TextureUsage::Sampled; // may be sampled
            if (rhi::IsDepthFormat(textureDesc.format))
            {
                rhiDesc.usage = rhiDesc.usage | rhi::TextureUsage::DepthStencil;
            }
            else
            {
                rhiDesc.usage = rhiDesc.usage | rhi::TextureUsage::RenderTarget;
            }

            // All-or-nothing: assign the members only once every view is created. Leaving `texture`
            // set with a null `textureView` (the old behaviour on a view-create failure) poisons the
            // transient pool and desyncs render-pass attachments (see ReturnTransientResources /
            // ExecuteRenderPass) - the viewport-resize corruption cascade.
            rhi::Texture* tex = nullptr;
            if (!device.CreateTexture(rhiDesc, tex).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }

            rhi::TextureView* view = nullptr;
            if (!device.CreateTextureView(tex, rhi::TextureViewDesc{}, view).IsOk())
            {
                device.DestroyTexture(tex);
                return Status{ErrorCode::Unknown};
            }

            rhi::TextureView* depthOnly = nullptr;
            if (rhi::IsDepthFormat(textureDesc.format) && rhi::HasStencil(textureDesc.format))
            {
                rhi::TextureViewDesc depthDesc{};
                depthDesc.aspect = rhi::TextureAspect::DepthOnly;
                depthDesc.label = u8"RGDepthOnlyView";
                if (!device.CreateTextureView(tex, depthDesc, depthOnly).IsOk())
                {
                    device.DestroyTextureView(view);
                    device.DestroyTexture(tex);
                    return Status{ErrorCode::Unknown};
                }
            }

            texture = tex;
            textureView = view;
            depthOnlyView = depthOnly;
            lastKnownState = tex->initialState;
            return Status{};
        }

        // Allocate GPU resources for a transient buffer.
        [[nodiscard]] Status AllocateBuffer(rhi::Device& device)
        {
            rhi::BufferDesc rhiDesc{};
            rhiDesc.size = bufferDesc.size;
            rhiDesc.usage = bufferDesc.usage;
            rhiDesc.label = name.AsView();

            rhi::Buffer* buf = nullptr;
            if (!device.CreateBuffer(rhiDesc, buf).IsOk())
            {
                return Status{ErrorCode::Unknown};
            }
            buffer = buf;
            lastKnownState = rhi::ResourceState::Undefined;
            return Status{};
        }

        // Release GPU resources for a transient resource (no-op otherwise).
        void ReleaseTransient(rhi::Device& device)
        {
            if (lifetime != RGResourceLifetime::Transient)
            {
                return;
            }
            if (depthOnlyView != nullptr)
            {
                device.DestroyTextureView(depthOnlyView);
            }
            if (textureView != nullptr)
            {
                device.DestroyTextureView(textureView);
            }
            if (texture != nullptr)
            {
                device.DestroyTexture(texture);
            }
            if (buffer != nullptr)
            {
                device.DestroyBuffer(buffer);
            }
        }

        [[nodiscard]] u32 TotalMipLevels() const
        {
            if (texture != nullptr)
            {
                return texture->desc.mipLevelCount;
            }
            if (resourceType == RGResourceType::Texture)
            {
                return textureDesc.mipLevelCount;
            }
            return 1;
        }
        [[nodiscard]] u32 TotalArrayLayers() const
        {
            if (texture != nullptr)
            {
                return texture->desc.arrayLayerCount;
            }
            if (resourceType == RGResourceType::Texture)
            {
                return textureDesc.arrayLayerCount;
            }
            return 1;
        }

        void ResetTracking()
        {
            refCount = 0;
            firstWriter = PassHandle::Invalid();
            lastReader = PassHandle::Invalid();
            firstUsePass = -1;
            lastUsePass = -1;
        }

        // --- identity / lifetime ---
        String name;
        RGResourceType resourceType;
        RGResourceLifetime lifetime;
        u32 generation = 1;

        // --- reference tracking (computed during compile) ---
        i32 refCount = 0;
        PassHandle firstWriter = PassHandle::Invalid();
        PassHandle lastReader = PassHandle::Invalid();
        i32 firstUsePass = -1; // for aliasing
        i32 lastUsePass = -1;

        // --- texture data ---
        RGTextureDesc textureDesc;
        rhi::Texture* texture = nullptr;
        rhi::TextureView* textureView = nullptr;
        rhi::TextureView* depthOnlyView = nullptr;
        // Stable id of the backing physical texture; changes when a transient is (re)allocated a
        // different texture (e.g. on resize). Consumers caching a bind group over textureView key on
        // this so a reused-address view does not alias a stale, destroyed texture.
        u64 textureGeneration = 0;

        // --- buffer data ---
        RGBufferDesc bufferDesc;
        rhi::Buffer* buffer = nullptr;

        // --- state tracking ---
        rhi::ResourceState lastKnownState = rhi::ResourceState::Undefined;
        Optional<rhi::ResourceState> finalState; // transition-to after last use (imported)
        bool readableAfterWrite = false;         // transition to ShaderRead after last writer

        // --- persistent data (null for transient/imported) ---
        UniquePtr<PersistentResource> persistentData;
    };
}
