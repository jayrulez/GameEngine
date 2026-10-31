// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// HLSL register binding shift configuration.

export module foundation.rhi.vulkan:binding_shifts;

import foundation.core;
import foundation.rhi;

using namespace foundation::core;

export namespace foundation::rhi::vk
{

    /// Maps HLSL register spaces to Vulkan descriptor bindings.
    /// DXC's -fvk-*-shift flags use these values when compiling HLSL to SPIR-V.
    struct BindingShifts
    {
        u32 cbvShift = 0;     ///< Constant buffer (b) register shift.
        u32 srvShift = 0;     ///< Shader resource view (t) register shift.
        u32 uavShift = 0;     ///< Unordered access view (u) register shift.
        u32 samplerShift = 0; ///< Sampler (s) register shift.

        /// Standard layout: CBV=0, SRV=100, UAV=200, Sampler=300, the engine-wide compact table
        /// that WebGPU also uses (binding indices must stay under WebGPU's maxBindingsPerBindGroup
        /// of 1000; Vulkan does not constrain binding indices, so nothing is lost). Matches
        /// shaders::BindingShifts::Standard().
        static constexpr BindingShifts standard() { return {0, 100, 200, 300}; }

        /// Applies the appropriate shift for a binding type.
        u32 apply(BindingType type, u32 binding) const
        {
            switch (type)
            {
            case BindingType::UniformBuffer:
                return binding + cbvShift;
            case BindingType::SampledTexture:
            case BindingType::BindlessTextures:
            case BindingType::AccelerationStructure:
            // A read-only StructuredBuffer is an SRV in HLSL (a `t` register), so it takes the SRV
            // shift, unlike RWStructuredBuffer (a `u` register, a UAV). DXC shifts the SPIR-V
            // binding accordingly, so the layout must use the same class to match.
            case BindingType::StorageBufferReadOnly:
                return binding + srvShift;
            case BindingType::StorageTextureReadOnly:
            case BindingType::StorageTextureReadWrite:
            case BindingType::BindlessStorageTextures:
            case BindingType::StorageBufferReadWrite:
            case BindingType::BindlessStorageBuffers:
                return binding + uavShift;
            case BindingType::Sampler:
            case BindingType::ComparisonSampler:
            case BindingType::BindlessSamplers:
                return binding + samplerShift;
            }
            return binding;
        }
    };

} // namespace foundation::rhi::vk
