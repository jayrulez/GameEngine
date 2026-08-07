/// Vulkan implementation of Surface.
/// Wraps VkSurfaceKHR + parent VkInstance for cleanup.

module;
#include "Draconic.Foundation/Prelude.h"

#include "VkIncludes.h"

export module draconic.rhi.vulkan:surface;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::vk
{

    class VkSurfaceImpl : public Surface
    {
    public:
        VkSurfaceImpl(VkSurfaceKHR surface, VkInstance instance)
            : m_surface(surface), m_instance(instance)
        {
        }

        [[nodiscard]] VkSurfaceKHR handle() const { return m_surface; }

        void Destroy()
        {
            if (m_surface != VK_NULL_HANDLE)
            {
                vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
                m_surface = VK_NULL_HANDLE;
            }
        }

    private:
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkInstance m_instance = VK_NULL_HANDLE;
    };

} // namespace draconic::rhi::vk
