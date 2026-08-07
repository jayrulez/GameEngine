// Draconic::RenderGraph - :persistent_resource partition
//
// A persistent resource that survives across frames with tracked state.
// Externally owned - the graph never creates or destroys these. The ping-pong
// variant carries two slots (current + previous frame) for temporal effects.
// Ported from Sedulous.RenderGraph (PersistentResource.bf).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.rendergraph:persistent_resource;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rendergraph
{
    namespace rhi = draconic::rhi;

    class PersistentResource
    {
    public:
        static constexpr i32 kSlotCount = 2; // current + one previous

        PersistentResource(rhi::Texture* texture, rhi::TextureView* view) noexcept
        {
            m_textures[0] = texture;
            m_views[0] = view;
        }

        // Ping-pong (double-buffered) variant.
        PersistentResource(rhi::Texture* tex0, rhi::Texture* tex1, rhi::TextureView* view0,
                           rhi::TextureView* view1) noexcept
            : m_isPingPong(true)
        {
            m_textures[0] = tex0;
            m_textures[1] = tex1;
            m_views[0] = view0;
            m_views[1] = view1;
        }

        [[nodiscard]] rhi::Texture* CurrentTexture() const noexcept
        {
            return m_textures[m_currentIndex];
        }
        [[nodiscard]] rhi::TextureView* CurrentView() const noexcept
        {
            return m_views[m_currentIndex];
        }
        [[nodiscard]] rhi::Texture* PreviousTexture() const noexcept
        {
            return m_isPingPong ? m_textures[(m_currentIndex + kSlotCount - 1) % kSlotCount]
                                : m_textures[m_currentIndex];
        }
        [[nodiscard]] rhi::TextureView* PreviousView() const noexcept
        {
            return m_isPingPong ? m_views[(m_currentIndex + kSlotCount - 1) % kSlotCount]
                                : m_views[m_currentIndex];
        }
        [[nodiscard]] bool IsPingPong() const noexcept { return m_isPingPong; }

        void Swap() noexcept
        {
            if (m_isPingPong)
            {
                m_currentIndex = (m_currentIndex + 1) % kSlotCount;
            }
        }

        // Re-point the active slot (e.g. when the external texture is recreated on resize).
        void UpdateTexture(rhi::Texture* texture, rhi::TextureView* view) noexcept
        {
            m_textures[m_currentIndex] = texture;
            m_views[m_currentIndex] = view;
        }

        // Cross-frame tracking the graph reads/writes (persists across Reset()).
        bool firstFrame = true;
        rhi::ResourceState lastKnownState = rhi::ResourceState::Undefined;
        // Non-empty when the resource ended a frame in a non-uniform state;
        // otherwise lastKnownState is the uniform value.
        Array<rhi::ResourceState> subresourceStates;

    private:
        rhi::Texture* m_textures[kSlotCount] = {};
        rhi::TextureView* m_views[kSlotCount] = {};
        i32 m_currentIndex = 0;
        bool m_isPingPong = false;
    };
}
