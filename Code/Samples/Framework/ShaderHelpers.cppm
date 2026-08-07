/// Shader compilation helpers - wraps DXC compile + createShaderModule into one call.
/// Automatically selects SPIR-V (Vulkan) or DXIL (DX12) based on device type.
/// Applies Vulkan binding shifts when targeting SPIR-V.

export module draconic.samples.framework:shader_helpers;

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;

using namespace draconic::foundation;

export namespace draconic::samples::framework
{

    /// Compile HLSL source to a ShaderModule with a specific shader model.
    inline Status CompileToModule(shaders::Compiler* compiler, rhi::Device* device,
                                  StringView hlslSource, shaders::ShaderStage stage,
                                  StringView entryPoint, StringView label, StringView shaderModel,
                                  rhi::ShaderModule*& out)
    {
        out = nullptr;

        bool isDX12 = (device->type == rhi::DeviceType::DX12);
        auto target = isDX12 ? shaders::ShaderTarget::DXIL : shaders::ShaderTarget::SPIRV;

        shaders::CompileOptions opts{};
        opts.shaderModel = shaderModel;
        opts.optimizationLevel = 3;

        // SPIR-V targets need binding shifts; DX12 uses register spaces natively.
        // ONE engine-wide table (WebGPU-compatible compact values) serves Vulkan and
        // WebGPU alike - see shaders::BindingShifts::Standard().
        if (!isDX12)
        {
            opts.bindingShifts = shaders::BindingShifts::Standard();
            if (device->type == rhi::DeviceType::WebGPU)
            {
                opts.spirvTargetEnvironment = u8"vulkan1.1"; // naga rejects SPIR-V 1.4+
            }
            opts.bindingShiftSets = 4;
        }

        shaders::CompileResult cr{};
        Status r = compiler->compile(reinterpret_cast<const u8*>(hlslSource.Data()),
                                     hlslSource.Size(), stage, entryPoint, target, opts, cr);

        if (r != ErrorCode::Ok)
        {
            if (cr.messages)
            {
                const String lbl = String(label);
                rhi::LogErrorf("Shader compile failed (%s): %s",
                               reinterpret_cast<const char*>(lbl.CStr()), cr.messages);
            }
            compiler->freeResult(cr);
            return ErrorCode::Unknown;
        }

        rhi::ShaderModuleDesc desc{};
        desc.code = Span<const u8>(cr.bytecode, cr.bytecodeSize);
        desc.label = label;
        r = device->CreateShaderModule(desc, out);

        compiler->freeResult(cr);
        return r;
    }

    /// Compile HLSL source to a ShaderModule. Default shader model 6.0.
    inline Status CompileToModule(shaders::Compiler* compiler, rhi::Device* device,
                                  StringView hlslSource, shaders::ShaderStage stage,
                                  StringView entryPoint, StringView label, rhi::ShaderModule*& out)
    {
        return CompileToModule(compiler, device, hlslSource, stage, entryPoint, label, u8"6_0",
                               out);
    }

} // namespace draconic::samples::framework
