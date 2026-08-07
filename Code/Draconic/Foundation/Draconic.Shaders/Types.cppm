/// Shader compilation types.

export module draconic.shaders:types;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::shaders
{

    enum class ShaderStage : u32
    {
        Vertex,
        Fragment,
        Compute,
        Mesh,
        Task,
        RayGen,
        ClosestHit,
        AnyHit,
        Miss,
        Intersection,
        Callable,
    };

    enum class ShaderTarget : u32
    {
        SPIRV,
        DXIL,
    };

    struct ShaderDefine
    {
        StringView name;
        StringView value;
    };

    struct BindingShifts
    {
        u32 constantBufferShift = 0;
        u32 textureShift = 0;
        u32 samplerShift = 0;
        u32 uavShift = 0;

        /// THE register-space shift table, one value engine-wide: CBV=0, SRV=+100,
        /// UAV=+200, Sampler=+300. Compact so binding indices stay under WebGPU's
        /// maxBindingsPerBindGroup (1000 in browsers, non-negotiable); Vulkan places
        /// no constraint on binding indices, so the same table serves both SPIR-V
        /// backends. Must match rhi::vk::BindingShifts::standard() and the WebGPU
        /// backend's ShiftedBinding constants.
        static constexpr BindingShifts Standard() { return {0, 100, 300, 200}; }
    };

    struct CompileOptions
    {
        StringView shaderModel = u8"6_0";
        /// SPIR-V target environment (-fspv-target-env). vulkan1.3 for the Vulkan
        /// backend; WebGPU compiles pass vulkan1.1 - naga's SPIR-V frontend rejects
        /// SPIR-V 1.4+ instructions (OpCopyLogical), which DXC emits above 1.1.
        StringView spirvTargetEnvironment = u8"vulkan1.3";
        i32 optimizationLevel = 3;
        bool enableDebugInfo = false;
        bool rowMajorMatrices = false;
        Span<const ShaderDefine> defines;
        Span<const StringView> includePaths;
        BindingShifts bindingShifts;
        u32 bindingShiftSets = 1;
    };

    struct CompileResult
    {
        u8* bytecode = nullptr;
        usize bytecodeSize = 0;
        char* messages = nullptr;
        usize messagesSize = 0;
        bool success = false;
    };

} // namespace draconic::shaders
