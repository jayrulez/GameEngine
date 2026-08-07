/// draconic.rhi.webgpu:conversions - RHI enum/format -> WebGPU translations.
///
/// The RHI's TextureFormat deliberately follows WebGPU conventions, so the format
/// table is near-1:1. Genuine gaps map to Undefined and the caller fails with
/// NotSupported: RGBA16Unorm/Snorm (native-extension formats, not in webgpu.h's
/// standard enum). AddressMode::ClampToBorder narrows to ClampToEdge (core WebGPU
/// has no border sampling) - visible difference only on shadow-map edge taps.

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:conversions;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    [[nodiscard]] inline WGPUTextureFormat ToWgpuTextureFormat(TextureFormat format)
    {
        switch (format)
        {
        case TextureFormat::Undefined:            return WGPUTextureFormat_Undefined;
        case TextureFormat::R8Unorm:              return WGPUTextureFormat_R8Unorm;
        case TextureFormat::R8Snorm:              return WGPUTextureFormat_R8Snorm;
        case TextureFormat::R8Uint:               return WGPUTextureFormat_R8Uint;
        case TextureFormat::R8Sint:               return WGPUTextureFormat_R8Sint;
        case TextureFormat::R16Uint:              return WGPUTextureFormat_R16Uint;
        case TextureFormat::R16Sint:              return WGPUTextureFormat_R16Sint;
        case TextureFormat::R16Float:             return WGPUTextureFormat_R16Float;
        case TextureFormat::RG8Unorm:             return WGPUTextureFormat_RG8Unorm;
        case TextureFormat::RG8Snorm:             return WGPUTextureFormat_RG8Snorm;
        case TextureFormat::RG8Uint:              return WGPUTextureFormat_RG8Uint;
        case TextureFormat::RG8Sint:              return WGPUTextureFormat_RG8Sint;
        case TextureFormat::R32Uint:              return WGPUTextureFormat_R32Uint;
        case TextureFormat::R32Sint:              return WGPUTextureFormat_R32Sint;
        case TextureFormat::R32Float:             return WGPUTextureFormat_R32Float;
        case TextureFormat::RG16Uint:             return WGPUTextureFormat_RG16Uint;
        case TextureFormat::RG16Sint:             return WGPUTextureFormat_RG16Sint;
        case TextureFormat::RG16Float:            return WGPUTextureFormat_RG16Float;
        case TextureFormat::RGBA8Unorm:           return WGPUTextureFormat_RGBA8Unorm;
        case TextureFormat::RGBA8UnormSrgb:       return WGPUTextureFormat_RGBA8UnormSrgb;
        case TextureFormat::RGBA8Snorm:           return WGPUTextureFormat_RGBA8Snorm;
        case TextureFormat::RGBA8Uint:            return WGPUTextureFormat_RGBA8Uint;
        case TextureFormat::RGBA8Sint:            return WGPUTextureFormat_RGBA8Sint;
        case TextureFormat::BGRA8Unorm:           return WGPUTextureFormat_BGRA8Unorm;
        case TextureFormat::BGRA8UnormSrgb:       return WGPUTextureFormat_BGRA8UnormSrgb;
        case TextureFormat::RGB10A2Unorm:         return WGPUTextureFormat_RGB10A2Unorm;
        case TextureFormat::RGB10A2Uint:          return WGPUTextureFormat_RGB10A2Uint;
        case TextureFormat::RG11B10Float:         return WGPUTextureFormat_RG11B10Ufloat;
        case TextureFormat::RGB9E5Float:          return WGPUTextureFormat_RGB9E5Ufloat;
        case TextureFormat::RG32Uint:             return WGPUTextureFormat_RG32Uint;
        case TextureFormat::RG32Sint:             return WGPUTextureFormat_RG32Sint;
        case TextureFormat::RG32Float:            return WGPUTextureFormat_RG32Float;
        case TextureFormat::RGBA16Uint:           return WGPUTextureFormat_RGBA16Uint;
        case TextureFormat::RGBA16Sint:           return WGPUTextureFormat_RGBA16Sint;
        case TextureFormat::RGBA16Float:          return WGPUTextureFormat_RGBA16Float;
        case TextureFormat::RGBA16Unorm:          return WGPUTextureFormat_Undefined; // native-ext only
        case TextureFormat::RGBA16Snorm:          return WGPUTextureFormat_Undefined; // native-ext only
        case TextureFormat::RGBA32Uint:           return WGPUTextureFormat_RGBA32Uint;
        case TextureFormat::RGBA32Sint:           return WGPUTextureFormat_RGBA32Sint;
        case TextureFormat::RGBA32Float:          return WGPUTextureFormat_RGBA32Float;
        case TextureFormat::Depth16Unorm:         return WGPUTextureFormat_Depth16Unorm;
        case TextureFormat::Depth24Plus:          return WGPUTextureFormat_Depth24Plus;
        case TextureFormat::Depth24PlusStencil8:  return WGPUTextureFormat_Depth24PlusStencil8;
        case TextureFormat::Depth32Float:         return WGPUTextureFormat_Depth32Float;
        case TextureFormat::Depth32FloatStencil8: return WGPUTextureFormat_Depth32FloatStencil8;
        case TextureFormat::Stencil8:             return WGPUTextureFormat_Stencil8;
        case TextureFormat::BC1RGBAUnorm:         return WGPUTextureFormat_BC1RGBAUnorm;
        case TextureFormat::BC1RGBAUnormSrgb:     return WGPUTextureFormat_BC1RGBAUnormSrgb;
        case TextureFormat::BC2RGBAUnorm:         return WGPUTextureFormat_BC2RGBAUnorm;
        case TextureFormat::BC2RGBAUnormSrgb:     return WGPUTextureFormat_BC2RGBAUnormSrgb;
        case TextureFormat::BC3RGBAUnorm:         return WGPUTextureFormat_BC3RGBAUnorm;
        case TextureFormat::BC3RGBAUnormSrgb:     return WGPUTextureFormat_BC3RGBAUnormSrgb;
        case TextureFormat::BC4RUnorm:            return WGPUTextureFormat_BC4RUnorm;
        case TextureFormat::BC4RSnorm:            return WGPUTextureFormat_BC4RSnorm;
        case TextureFormat::BC5RGUnorm:           return WGPUTextureFormat_BC5RGUnorm;
        case TextureFormat::BC5RGSnorm:           return WGPUTextureFormat_BC5RGSnorm;
        case TextureFormat::BC6HRGBUfloat:        return WGPUTextureFormat_BC6HRGBUfloat;
        case TextureFormat::BC6HRGBFloat:         return WGPUTextureFormat_BC6HRGBFloat;
        case TextureFormat::BC7RGBAUnorm:         return WGPUTextureFormat_BC7RGBAUnorm;
        case TextureFormat::BC7RGBAUnormSrgb:     return WGPUTextureFormat_BC7RGBAUnormSrgb;
        default:                                  return WGPUTextureFormat_Undefined;
        }
    }

    [[nodiscard]] inline WGPUBufferUsage ToWgpuBufferUsage(BufferUsage usage,
                                                           MemoryLocation memory)
    {
        WGPUBufferUsage out = WGPUBufferUsage_None;
        if (HasFlag(usage, BufferUsage::CopySrc))
        {
            out |= WGPUBufferUsage_CopySrc;
        }
        if (HasFlag(usage, BufferUsage::CopyDst))
        {
            out |= WGPUBufferUsage_CopyDst;
        }
        if (HasFlag(usage, BufferUsage::Vertex))
        {
            out |= WGPUBufferUsage_Vertex;
        }
        if (HasFlag(usage, BufferUsage::Index))
        {
            out |= WGPUBufferUsage_Index;
        }
        if (HasFlag(usage, BufferUsage::Uniform))
        {
            out |= WGPUBufferUsage_Uniform;
        }
        if (HasFlag(usage, BufferUsage::Storage) || HasFlag(usage, BufferUsage::StorageRead))
        {
            out |= WGPUBufferUsage_Storage;
        }
        if (HasFlag(usage, BufferUsage::Indirect))
        {
            out |= WGPUBufferUsage_Indirect;
        }

        // The Map emulation's transport (see :buffer): CPU->GPU shadows upload through
        // WriteBuffer (CopyDst); GPU->CPU readback maps for real, which WebGPU only
        // validates as MapRead|CopyDst - nothing else may be combined with MapRead.
        if (memory == MemoryLocation::CpuToGpu)
        {
            out |= WGPUBufferUsage_CopyDst;
        }
        else if (memory == MemoryLocation::GpuToCpu)
        {
            out = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        }
        return out;
    }

    [[nodiscard]] inline WGPUTextureUsage ToWgpuTextureUsage(TextureUsage usage)
    {
        const auto HasFlag = [usage](TextureUsage flag)
        { return (static_cast<u32>(usage) & static_cast<u32>(flag)) != 0; };
        WGPUTextureUsage out = WGPUTextureUsage_None;
        if (HasFlag(TextureUsage::CopySrc))
        {
            out |= WGPUTextureUsage_CopySrc;
        }
        if (HasFlag(TextureUsage::CopyDst))
        {
            out |= WGPUTextureUsage_CopyDst;
        }
        if (HasFlag(TextureUsage::Sampled))
        {
            out |= WGPUTextureUsage_TextureBinding;
        }
        if (HasFlag(TextureUsage::Storage))
        {
            out |= WGPUTextureUsage_StorageBinding;
        }
        if (HasFlag(TextureUsage::RenderTarget) ||
            HasFlag(TextureUsage::DepthStencil))
        {
            out |= WGPUTextureUsage_RenderAttachment;
        }
        // InputAttachment is a Vulkan concept; sampling covers its WebGPU shape.
        if (HasFlag(TextureUsage::InputAttachment))
        {
            out |= WGPUTextureUsage_TextureBinding;
        }
        return out;
    }

    [[nodiscard]] inline WGPUTextureDimension ToWgpuTextureDimension(TextureDimension dimension)
    {
        switch (dimension)
        {
        case TextureDimension::Texture1D:
            return WGPUTextureDimension_1D;
        case TextureDimension::Texture2D:
            return WGPUTextureDimension_2D;
        case TextureDimension::Texture3D:
            return WGPUTextureDimension_3D;
        }
        return WGPUTextureDimension_2D;
    }

    [[nodiscard]] inline WGPUTextureViewDimension
    ToWgpuTextureViewDimension(TextureViewDimension dimension)
    {
        switch (dimension)
        {
        case TextureViewDimension::Texture1D:
            return WGPUTextureViewDimension_1D;
        case TextureViewDimension::Texture1DArray:
            return WGPUTextureViewDimension_Undefined; // no 1D arrays in WebGPU
        case TextureViewDimension::Texture2D:
            return WGPUTextureViewDimension_2D;
        case TextureViewDimension::Texture2DArray:
            return WGPUTextureViewDimension_2DArray;
        case TextureViewDimension::TextureCube:
            return WGPUTextureViewDimension_Cube;
        case TextureViewDimension::TextureCubeArray:
            return WGPUTextureViewDimension_CubeArray;
        case TextureViewDimension::Texture3D:
            return WGPUTextureViewDimension_3D;
        }
        return WGPUTextureViewDimension_2D;
    }

    [[nodiscard]] inline WGPUTextureAspect ToWgpuTextureAspect(TextureAspect aspect)
    {
        switch (aspect)
        {
        case TextureAspect::All:
            return WGPUTextureAspect_All;
        case TextureAspect::DepthOnly:
            return WGPUTextureAspect_DepthOnly;
        case TextureAspect::StencilOnly:
            return WGPUTextureAspect_StencilOnly;
        }
        return WGPUTextureAspect_All;
    }

    [[nodiscard]] inline WGPUFilterMode ToWgpuFilterMode(FilterMode filter)
    {
        return filter == FilterMode::Nearest ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
    }

    [[nodiscard]] inline WGPUMipmapFilterMode ToWgpuMipmapFilterMode(MipmapFilterMode filter)
    {
        return filter == MipmapFilterMode::Nearest ? WGPUMipmapFilterMode_Nearest
                                                   : WGPUMipmapFilterMode_Linear;
    }

    [[nodiscard]] inline WGPUAddressMode ToWgpuAddressMode(AddressMode mode)
    {
        switch (mode)
        {
        case AddressMode::Repeat:
            return WGPUAddressMode_Repeat;
        case AddressMode::MirrorRepeat:
            return WGPUAddressMode_MirrorRepeat;
        case AddressMode::ClampToEdge:
            return WGPUAddressMode_ClampToEdge;
        case AddressMode::ClampToBorder:
            // Core WebGPU has no border sampling; edge clamp is the nearest behavior.
            return WGPUAddressMode_ClampToEdge;
        }
        return WGPUAddressMode_Repeat;
    }

    [[nodiscard]] inline WGPUCompareFunction ToWgpuCompareFunction(CompareFunction function)
    {
        switch (function)
        {
        case CompareFunction::Never:
            return WGPUCompareFunction_Never;
        case CompareFunction::Less:
            return WGPUCompareFunction_Less;
        case CompareFunction::Equal:
            return WGPUCompareFunction_Equal;
        case CompareFunction::LessEqual:
            return WGPUCompareFunction_LessEqual;
        case CompareFunction::Greater:
            return WGPUCompareFunction_Greater;
        case CompareFunction::NotEqual:
            return WGPUCompareFunction_NotEqual;
        case CompareFunction::GreaterEqual:
            return WGPUCompareFunction_GreaterEqual;
        case CompareFunction::Always:
            return WGPUCompareFunction_Always;
        }
        return WGPUCompareFunction_Always;
    }

    /// True for formats the internal blit pass can render to (2D color targets):
    /// excludes depth/stencil, block-compressed, and formats WebGPU cannot render.
    [[nodiscard]] inline bool IsBlitCapableFormat(TextureFormat format)
    {
        switch (format)
        {
        case TextureFormat::Undefined:
        case TextureFormat::Depth16Unorm:
        case TextureFormat::Depth24Plus:
        case TextureFormat::Depth24PlusStencil8:
        case TextureFormat::Depth32Float:
        case TextureFormat::Depth32FloatStencil8:
        case TextureFormat::Stencil8:
        case TextureFormat::BC1RGBAUnorm:
        case TextureFormat::BC1RGBAUnormSrgb:
        case TextureFormat::BC2RGBAUnorm:
        case TextureFormat::BC2RGBAUnormSrgb:
        case TextureFormat::BC3RGBAUnorm:
        case TextureFormat::BC3RGBAUnormSrgb:
        case TextureFormat::BC4RUnorm:
        case TextureFormat::BC4RSnorm:
        case TextureFormat::BC5RGUnorm:
        case TextureFormat::BC5RGSnorm:
        case TextureFormat::BC6HRGBUfloat:
        case TextureFormat::BC6HRGBFloat:
        case TextureFormat::BC7RGBAUnorm:
        case TextureFormat::BC7RGBAUnormSrgb:
        case TextureFormat::RGBA16Unorm:
        case TextureFormat::RGBA16Snorm:
        case TextureFormat::RGB9E5Float:
        case TextureFormat::RG11B10Float:
            return false;
        default:
            return true;
        }
    }

    /// Label helper: RHI labels are UTF-8 StringViews, WebGPU wants {data,length}.
    [[nodiscard]] inline WGPUStringView ToWgpuStringView(StringView label)
    {
        return WGPUStringView{reinterpret_cast<const char*>(label.Data()), label.Size()};
    }

    /// The engine-wide register-space shift table: CBV=0, SRV=+100, UAV=+200,
    /// Sampler=+300 (shaders::BindingShifts::Standard(), shared with Vulkan - compact
    /// because WebGPU validates binding indices against maxBindingsPerBindGroup, 1000
    /// in browsers). DXC bakes these into the SPIR-V, so layouts must declare the
    /// same numbers. The cook-time WGSL path (browser) will emit compact bindings
    /// and bypass shifting entirely.
    inline constexpr u32 kSrvBindingShift = 100;
    inline constexpr u32 kUavBindingShift = 200;
    inline constexpr u32 kSamplerBindingShift = 300;

    [[nodiscard]] inline u32 ShiftedBinding(BindingType type, u32 binding)
    {
        switch (type)
        {
        case BindingType::UniformBuffer:
            return binding;
        case BindingType::SampledTexture:
        case BindingType::StorageBufferReadOnly: // HLSL StructuredBuffer = SRV (t register)
            return binding + kSrvBindingShift;
        case BindingType::StorageTextureReadOnly:
        case BindingType::StorageTextureReadWrite:
        case BindingType::StorageBufferReadWrite:
            return binding + kUavBindingShift;
        case BindingType::Sampler:
        case BindingType::ComparisonSampler:
            return binding + kSamplerBindingShift;
        default:
            return binding; // bindless / accel-struct types never reach WebGPU layouts
        }
    }

    [[nodiscard]] inline WGPUShaderStage ToWgpuShaderStage(ShaderStage stages)
    {
        WGPUShaderStage out = WGPUShaderStage_None;
        const u32 bits = static_cast<u32>(stages);
        if ((bits & static_cast<u32>(ShaderStage::Vertex)) != 0)
        {
            out |= WGPUShaderStage_Vertex;
        }
        if ((bits & static_cast<u32>(ShaderStage::Fragment)) != 0)
        {
            out |= WGPUShaderStage_Fragment;
        }
        if ((bits & static_cast<u32>(ShaderStage::Compute)) != 0)
        {
            out |= WGPUShaderStage_Compute;
        }
        return out;
    }

    [[nodiscard]] inline WGPUPrimitiveTopology ToWgpuPrimitiveTopology(PrimitiveTopology t)
    {
        switch (t)
        {
        case PrimitiveTopology::PointList:     return WGPUPrimitiveTopology_PointList;
        case PrimitiveTopology::LineList:      return WGPUPrimitiveTopology_LineList;
        case PrimitiveTopology::LineStrip:     return WGPUPrimitiveTopology_LineStrip;
        case PrimitiveTopology::TriangleList:  return WGPUPrimitiveTopology_TriangleList;
        case PrimitiveTopology::TriangleStrip: return WGPUPrimitiveTopology_TriangleStrip;
        }
        return WGPUPrimitiveTopology_TriangleList;
    }

    [[nodiscard]] inline WGPUFrontFace ToWgpuFrontFace(FrontFace face)
    {
        return face == FrontFace::CCW ? WGPUFrontFace_CCW : WGPUFrontFace_CW;
    }

    [[nodiscard]] inline WGPUCullMode ToWgpuCullMode(CullMode mode)
    {
        switch (mode)
        {
        case CullMode::None:  return WGPUCullMode_None;
        case CullMode::Front: return WGPUCullMode_Front;
        case CullMode::Back:  return WGPUCullMode_Back;
        }
        return WGPUCullMode_None;
    }

    [[nodiscard]] inline WGPUStencilOperation ToWgpuStencilOperation(StencilOperation op)
    {
        switch (op)
        {
        case StencilOperation::Keep:           return WGPUStencilOperation_Keep;
        case StencilOperation::Zero:           return WGPUStencilOperation_Zero;
        case StencilOperation::Replace:        return WGPUStencilOperation_Replace;
        case StencilOperation::IncrementClamp: return WGPUStencilOperation_IncrementClamp;
        case StencilOperation::DecrementClamp: return WGPUStencilOperation_DecrementClamp;
        case StencilOperation::Invert:         return WGPUStencilOperation_Invert;
        case StencilOperation::IncrementWrap:  return WGPUStencilOperation_IncrementWrap;
        case StencilOperation::DecrementWrap:  return WGPUStencilOperation_DecrementWrap;
        }
        return WGPUStencilOperation_Keep;
    }

    [[nodiscard]] inline WGPUBlendFactor ToWgpuBlendFactor(BlendFactor factor)
    {
        switch (factor)
        {
        case BlendFactor::Zero:              return WGPUBlendFactor_Zero;
        case BlendFactor::One:               return WGPUBlendFactor_One;
        case BlendFactor::Src:               return WGPUBlendFactor_Src;
        case BlendFactor::OneMinusSrc:       return WGPUBlendFactor_OneMinusSrc;
        case BlendFactor::SrcAlpha:          return WGPUBlendFactor_SrcAlpha;
        case BlendFactor::OneMinusSrcAlpha:  return WGPUBlendFactor_OneMinusSrcAlpha;
        case BlendFactor::Dst:               return WGPUBlendFactor_Dst;
        case BlendFactor::OneMinusDst:       return WGPUBlendFactor_OneMinusDst;
        case BlendFactor::DstAlpha:          return WGPUBlendFactor_DstAlpha;
        case BlendFactor::OneMinusDstAlpha:  return WGPUBlendFactor_OneMinusDstAlpha;
        case BlendFactor::SrcAlphaSaturated: return WGPUBlendFactor_SrcAlphaSaturated;
        case BlendFactor::Constant:          return WGPUBlendFactor_Constant;
        case BlendFactor::OneMinusConstant:  return WGPUBlendFactor_OneMinusConstant;
        }
        return WGPUBlendFactor_One;
    }

    [[nodiscard]] inline WGPUBlendOperation ToWgpuBlendOperation(BlendOperation op)
    {
        switch (op)
        {
        case BlendOperation::Add:             return WGPUBlendOperation_Add;
        case BlendOperation::Subtract:        return WGPUBlendOperation_Subtract;
        case BlendOperation::ReverseSubtract: return WGPUBlendOperation_ReverseSubtract;
        case BlendOperation::Min:             return WGPUBlendOperation_Min;
        case BlendOperation::Max:             return WGPUBlendOperation_Max;
        }
        return WGPUBlendOperation_Add;
    }

    [[nodiscard]] inline WGPUColorWriteMask ToWgpuColorWriteMask(ColorWriteMask mask)
    {
        WGPUColorWriteMask out = WGPUColorWriteMask_None;
        const u8 bits = static_cast<u8>(mask);
        if ((bits & static_cast<u8>(ColorWriteMask::Red)) != 0)
        {
            out |= WGPUColorWriteMask_Red;
        }
        if ((bits & static_cast<u8>(ColorWriteMask::Green)) != 0)
        {
            out |= WGPUColorWriteMask_Green;
        }
        if ((bits & static_cast<u8>(ColorWriteMask::Blue)) != 0)
        {
            out |= WGPUColorWriteMask_Blue;
        }
        if ((bits & static_cast<u8>(ColorWriteMask::Alpha)) != 0)
        {
            out |= WGPUColorWriteMask_Alpha;
        }
        return out;
    }

    [[nodiscard]] inline WGPUVertexFormat ToWgpuVertexFormat(VertexFormat format)
    {
        switch (format)
        {
        case VertexFormat::Uint8x2:   return WGPUVertexFormat_Uint8x2;
        case VertexFormat::Uint8x4:   return WGPUVertexFormat_Uint8x4;
        case VertexFormat::Sint8x2:   return WGPUVertexFormat_Sint8x2;
        case VertexFormat::Sint8x4:   return WGPUVertexFormat_Sint8x4;
        case VertexFormat::Unorm8x2:  return WGPUVertexFormat_Unorm8x2;
        case VertexFormat::Unorm8x4:  return WGPUVertexFormat_Unorm8x4;
        case VertexFormat::Snorm8x2:  return WGPUVertexFormat_Snorm8x2;
        case VertexFormat::Snorm8x4:  return WGPUVertexFormat_Snorm8x4;
        case VertexFormat::Uint16x2:  return WGPUVertexFormat_Uint16x2;
        case VertexFormat::Uint16x4:  return WGPUVertexFormat_Uint16x4;
        case VertexFormat::Sint16x2:  return WGPUVertexFormat_Sint16x2;
        case VertexFormat::Sint16x4:  return WGPUVertexFormat_Sint16x4;
        case VertexFormat::Unorm16x2: return WGPUVertexFormat_Unorm16x2;
        case VertexFormat::Unorm16x4: return WGPUVertexFormat_Unorm16x4;
        case VertexFormat::Snorm16x2: return WGPUVertexFormat_Snorm16x2;
        case VertexFormat::Snorm16x4: return WGPUVertexFormat_Snorm16x4;
        case VertexFormat::Float16x2: return WGPUVertexFormat_Float16x2;
        case VertexFormat::Float16x4: return WGPUVertexFormat_Float16x4;
        case VertexFormat::Float32:   return WGPUVertexFormat_Float32;
        case VertexFormat::Float32x2: return WGPUVertexFormat_Float32x2;
        case VertexFormat::Float32x3: return WGPUVertexFormat_Float32x3;
        case VertexFormat::Float32x4: return WGPUVertexFormat_Float32x4;
        case VertexFormat::Uint32:    return WGPUVertexFormat_Uint32;
        case VertexFormat::Uint32x2:  return WGPUVertexFormat_Uint32x2;
        case VertexFormat::Uint32x3:  return WGPUVertexFormat_Uint32x3;
        case VertexFormat::Uint32x4:  return WGPUVertexFormat_Uint32x4;
        case VertexFormat::Sint32:    return WGPUVertexFormat_Sint32;
        case VertexFormat::Sint32x2:  return WGPUVertexFormat_Sint32x2;
        case VertexFormat::Sint32x3:  return WGPUVertexFormat_Sint32x3;
        case VertexFormat::Sint32x4:  return WGPUVertexFormat_Sint32x4;
        }
        return WGPUVertexFormat_Float32;
    }

    [[nodiscard]] inline WGPUVertexStepMode ToWgpuVertexStepMode(VertexStepMode mode)
    {
        return mode == VertexStepMode::Instance ? WGPUVertexStepMode_Instance
                                                : WGPUVertexStepMode_Vertex;
    }

    [[nodiscard]] inline WGPUTextureSampleType ToWgpuTextureSampleType(TextureSampleType type)
    {
        switch (type)
        {
        case TextureSampleType::Float:             return WGPUTextureSampleType_Float;
        case TextureSampleType::UnfilterableFloat: return WGPUTextureSampleType_UnfilterableFloat;
        case TextureSampleType::Depth:             return WGPUTextureSampleType_Depth;
        case TextureSampleType::Uint:              return WGPUTextureSampleType_Uint;
        case TextureSampleType::Sint:              return WGPUTextureSampleType_Sint;
        }
        return WGPUTextureSampleType_Float;
    }

    [[nodiscard]] inline WGPULoadOp ToWgpuLoadOp(LoadOp op)
    {
        switch (op)
        {
        case LoadOp::Load:     return WGPULoadOp_Load;
        case LoadOp::Clear:    return WGPULoadOp_Clear;
        case LoadOp::DontCare: return WGPULoadOp_Clear; // WebGPU has no dont-care load
        }
        return WGPULoadOp_Clear;
    }

    [[nodiscard]] inline WGPUStoreOp ToWgpuStoreOp(StoreOp op)
    {
        return op == StoreOp::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
    }

    [[nodiscard]] inline WGPUPresentMode ToWgpuPresentMode(PresentMode mode)
    {
        switch (mode)
        {
        case PresentMode::Immediate:   return WGPUPresentMode_Immediate;
        case PresentMode::Mailbox:     return WGPUPresentMode_Mailbox;
        case PresentMode::Fifo:        return WGPUPresentMode_Fifo;
        case PresentMode::FifoRelaxed: return WGPUPresentMode_FifoRelaxed;
        }
        return WGPUPresentMode_Fifo;
    }
}
