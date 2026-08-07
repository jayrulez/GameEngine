/// Reusable depth buffer helper for samples.

export module draconic.samples.framework:depth_buffer;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::samples::framework
{

    struct DepthBuffer
    {
        rhi::Texture* texture = nullptr;
        rhi::TextureView* view = nullptr;
        rhi::TextureFormat format = rhi::TextureFormat::Depth24PlusStencil8;

        Status Recreate(rhi::Device* device, u32 width, u32 height, u32 sampleCount = 1)
        {
            Destroy(device);
            auto desc =
                rhi::TextureDesc::DepthBuffer(format, width, height, sampleCount, u8"Depth");
            if (device->CreateTexture(desc, texture) != ErrorCode::Ok)
                return ErrorCode::Unknown;

            rhi::TextureViewDesc vd{};
            vd.format = format;
            vd.dimension = rhi::TextureViewDimension::Texture2D;
            vd.baseMipLevel = 0;
            vd.mipLevelCount = 1;
            vd.baseArrayLayer = 0;
            vd.arrayLayerCount = 1;
            vd.label = u8"DepthView";
            if (device->CreateTextureView(texture, vd, view) != ErrorCode::Ok)
            {
                device->DestroyTexture(texture);
                return ErrorCode::Unknown;
            }
            return ErrorCode::Ok;
        }

        void Destroy(rhi::Device* device)
        {
            if (view)
            {
                device->DestroyTextureView(view);
                view = nullptr;
            }
            if (texture)
            {
                device->DestroyTexture(texture);
                texture = nullptr;
            }
        }
    };

} // namespace draconic::samples::framework
