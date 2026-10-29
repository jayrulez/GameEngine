// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Shader compilation types.

export module foundation.shaders:types;

import foundation.core;

using namespace foundation::core;

export namespace foundation::shaders
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

    // The engine's own shaders (Data/Engine/Shaders) are a file per stage, `<name>.<vs|ps|cs>.hlsl`, whose
    // function is `main`: the corpus's convention, stated once for the provider that serves them
    // and the cook that packs them. A shader of another shape names its entries itself.
    inline constexpr StringView kCorpusEntryPoint = u8"main";

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

    /// Serves `#include` requests during a compile from wherever the caller's shader sources
    /// live (a VFS mount, memory, ...) - the compiler never touches the native filesystem for
    /// includes when one is set. `path` is the include path as the preprocessor formed it,
    /// normalized to forward slashes with any leading "./" stripped: relative to the including
    /// file's directory (the main source has none, so a first-level include arrives bare -
    /// "common.hlsli"), and nested includes carry the includer's directory prefix. Return
    /// false for "not found" (the preprocessor then reports the missing include).
    class IShaderIncludeResolver
    {
    public:
        virtual ~IShaderIncludeResolver() = default;
        [[nodiscard]] virtual bool LoadInclude(StringView path, String& outSource) = 0;
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
        /// Native include directories (-I): a quoted include is looked for beside its includer
        /// first, then in each of these in order, and read from disk. Ignored when
        /// `includeResolver` is set.
        Span<const StringView> includePaths;
        /// When set, EVERY include resolves through it (no disk access) - the dev shader
        /// provider serves includes from the data mount this way. Borrowed for the call.
        /// On both routes the compiler expands an include's `effect` blocks (:properties).
        IShaderIncludeResolver* includeResolver = nullptr;
        BindingShifts bindingShifts;
        u32 bindingShiftSets = 1;
        /// SPIR-V only: keep unused stage-interface variables (-fspv-preserve-interface). Set from
        /// a stage's `// preserve-interface` directive (ParsePreserveInterfaceDirective).
        bool preserveInterface = false;
        /// The source's file name as its author knows it (`Toon.hlsl`): DXC's messages name it
        /// (`Toon.hlsl:12:5: error: ...`) instead of a placeholder. Empty = DXC's default.
        StringView sourceName;
    };

    struct CompileResult
    {
        u8* bytecode = nullptr;
        usize bytecodeSize = 0;
        char* messages = nullptr;
        usize messagesSize = 0;
        bool success = false;
    };

} // namespace foundation::shaders
