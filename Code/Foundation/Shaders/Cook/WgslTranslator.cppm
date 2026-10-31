// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// WGSL cook: translates one HLSL shader stage to browser-conformant WGSL.
//
//   HLSL --DXC--> SPIR-V (vulkan1.1 + engine Standard binding shifts)
//        --naga--> WGSL
//        --tint--> validated (Chrome/Dawn conformance oracle)
//
// Cook time only (the export path), never at runtime: it shells out to the vendored naga and tint
// executables through core::RunProcess (paths baked in as BUILDSYSTEM_NAGA_PATH and
// BUILDSYSTEM_TINT_PATH by ThirdParty::ShaderCookTools in ThirdParty/CMakeLists.txt). naga is the
// version-matched translator (the same naga is wgpu-native's WGSL validator); tint errors on WGSL
// uniformity violations naga only warns on, so it is the second gate that catches
// browser-incompatible shaders at cook time.
//
// The intermediate SPIR-V and WGSL are written to a caller-provided scratch directory, since naga
// reads an input file and writes an output file (no stdin/stdout streaming for the emit path).

module;
#include "Core/Prelude.h"

export module foundation.shaders:wgsl_cook;

import foundation.core;
import :types;
import :compiler;
import :flags;
import :variants;

using namespace foundation::core;

export namespace foundation::shaders
{
    // Where translation stopped (Ok on success).
    enum class WgslCookStage
    {
        Ok,
        Compile,   // DXC HLSL -> SPIR-V failed
        Translate, // naga SPIR-V -> WGSL failed (or naga could not be run)
        Validate,  // tint rejected the WGSL (browser-incompatible) or could not be run
    };

    struct WgslCookResult
    {
        bool success = false;
        WgslCookStage failedStage = WgslCookStage::Ok;
        String wgsl;  // the translated WGSL, on success
        String error; // the tool diagnostic (DXC / naga / tint), on failure
    };

    // Translates HLSL shader stages to WGSL at cook time. Borrows a DXC Compiler (the SPIR-V front
    // end) and writes intermediates into `scratchDir` (must exist and be writable). naga and tint
    // default to the vendored binaries; a host with no vendored binary for its platform leaves the
    // path empty and Translate reports the tool missing (at the Translate stage for naga, at the
    // Validate stage for tint when validation is requested). Skipping validation is an explicit
    // opt-out, never silent.
    class WgslTranslator
    {
    public:
        WgslTranslator(Compiler& compiler, StringView scratchDir)
            : m_compiler(&compiler), m_scratchDir(scratchDir)
        {
#ifdef BUILDSYSTEM_NAGA_PATH
            m_naga = String(reinterpret_cast<const char8_t*>(BUILDSYSTEM_NAGA_PATH));
#endif
#ifdef BUILDSYSTEM_TINT_PATH
            m_tint = String(reinterpret_cast<const char8_t*>(BUILDSYSTEM_TINT_PATH));
#endif
        }

        void SetNagaPath(StringView p) { m_naga = String(p); }
        void SetTintPath(StringView p) { m_tint = String(p); }
        // Run tint over naga's output (default true). Off translates only, with no conformance gate.
        void SetValidateWithTint(bool v) { m_validate = v; }

        [[nodiscard]] bool HasNaga() const noexcept { return !m_naga.IsEmpty(); }
        [[nodiscard]] bool HasTint() const noexcept { return !m_tint.IsEmpty(); }

        // `entryPoint` is the stage's function in `hlsl`; the WGSL keeps its name (naga carries the
        // SPIR-V entry name through), so a pipeline asks for the same name on every backend.
        [[nodiscard]] WgslCookResult Translate(StringView hlsl, ShaderStage stage,
                                               StringView entryPoint,
                                               ShaderFlags flags = ShaderFlags::None,
                                               Span<const StringView> includePaths = {})
        {
            WgslCookResult result;

            if (m_naga.IsEmpty())
            {
                result.failedStage = WgslCookStage::Translate;
                result.error = String(u8"naga-cli not vendored for this host (BUILDSYSTEM_NAGA_PATH "
                                      u8"unset) - cannot translate to WGSL");
                return result;
            }

            // 1. DXC: HLSL -> SPIR-V with the WebGPU compile settings (the vulkan1.1 target env and
            //    the engine-wide Standard binding shifts), as ShaderSystem feeds the WebGPU backend.
            Array<ShaderDefine> defines;
            AppendDefines(flags, defines);
            // Browsers have no push constants and reject WGSL var<push_constant>. Compile the
            // PUSH_CONSTANT blocks as ordinary cbuffers (register(b0, spaceN)) so naga emits a
            // @group(spaceN) @binding(0) var<uniform> the browser accepts; the WebGPU backend feeds
            // it per draw through its push-constant emulation (see push_constant.hlsli).
            defines.PushBack(ShaderDefine{u8"PUSH_CONSTANT_AS_CBUFFER", u8"1"});
            CompileOptions opts{};
            opts.shaderModel = u8"6_0";
            opts.optimizationLevel = 3;
            opts.spirvTargetEnvironment = u8"vulkan1.1";
            opts.bindingShifts = BindingShifts::Standard();
            opts.bindingShiftSets = 4;
            opts.defines = Span<const ShaderDefine>(defines.Data(), defines.Size());
            opts.includePaths = includePaths;
            opts.preserveInterface = ParsePreserveInterfaceDirective(hlsl);

            CompileResult cr{};
            const Status st =
                m_compiler->compile(reinterpret_cast<const u8*>(hlsl.Data()), hlsl.Size(), stage,
                                    entryPoint, ShaderTarget::SPIRV, opts, cr);
            if (st != ErrorCode::Ok || !cr.success)
            {
                result.failedStage = WgslCookStage::Compile;
                result.error = cr.messages != nullptr
                                   ? String(reinterpret_cast<const char8_t*>(cr.messages))
                                   : String(u8"DXC compile failed");
                m_compiler->freeResult(cr);
                return result;
            }

            const u64 id = m_counter++;
            const String spvPath = ScratchPath(id, u8".spv");
            const String wgslPath = ScratchPath(id, u8".wgsl");

            const Status wroteSpv =
                WriteFile(spvPath.AsView(),
                          Span<const byte>(reinterpret_cast<const byte*>(cr.bytecode),
                                           cr.bytecodeSize));
            m_compiler->freeResult(cr);
            if (!wroteSpv.IsOk())
            {
                result.failedStage = WgslCookStage::Translate;
                result.error = String(u8"could not write intermediate SPIR-V to the scratch dir");
                return result;
            }

            // 2. naga: SPIR-V -> WGSL (writes wgslPath). --keep-coordinate-space is required:
            // without it naga bakes a clip-space Y adjustment into the WGSL that wgpu's runtime
            // SPIR-V frontend does not apply, so the cooked-WGSL path renders mirrored relative to
            // both the SPIR-V path and Vulkan (Render.Backend.Tests probes this). With the flag,
            // every WebGPU shader path shares Vulkan's raster orientation and the renderer needs
            // no Y-flip compensation (NeedsClipSpaceYFlip is false).
            const StringView nagaArgs[] = {u8"--keep-coordinate-space", spvPath.AsView(),
                                           wgslPath.AsView()};
            const ProcessResult np =
                RunProcess(m_naga.AsView(), Span<const StringView>(nagaArgs, 3));
            if (!np.Ok())
            {
                result.failedStage = WgslCookStage::Translate;
                if (np.Ran())
                {
                    result.error = np.output;
                }
                else
                {
                    result.error = String(u8"could not run naga: ");
                    result.error += m_naga;
                }
                Cleanup(spvPath, wgslPath);
                return result;
            }

            const Result<Array<byte>> wgslBytes = ReadFile(wgslPath.AsView());
            if (!wgslBytes.HasValue())
            {
                result.failedStage = WgslCookStage::Translate;
                result.error = String(u8"naga reported success but produced no WGSL output");
                Cleanup(spvPath, wgslPath);
                return result;
            }
            const Array<byte>& wb = wgslBytes.Value();
            result.wgsl = String(StringView(reinterpret_cast<const char8_t*>(wb.Data()), wb.Size()));

            // 3. tint: validate the WGSL against the Chrome/Dawn frontend (uniformity and the
            // rest). When validation is requested but no tint is vendored for this host, fail
            // rather than skip: unvalidated WGSL is the class of output the browser rejects at
            // runtime, and tint is vendored to gate it at cook time. A host that has no tint opts
            // out explicitly (SetValidateWithTint(false) or ShaderCookOptions::validateWgsl =
            // false) and owns the risk.
            if (m_validate && m_tint.IsEmpty())
            {
                result.failedStage = WgslCookStage::Validate;
                result.error = String(u8"tint is not vendored for this host - WGSL validation is "
                                      u8"unavailable (pass validateWgsl=false to cook unvalidated "
                                      u8"WGSL at your own risk)");
                Cleanup(spvPath, wgslPath);
                return result;
            }
            if (m_validate && !m_tint.IsEmpty())
            {
                const StringView tintArgs[] = {u8"--format", u8"wgsl", wgslPath.AsView()};
                const ProcessResult tp =
                    RunProcess(m_tint.AsView(), Span<const StringView>(tintArgs, 3));
                if (!tp.Ok())
                {
                    result.failedStage = WgslCookStage::Validate;
                    if (tp.Ran())
                    {
                        result.error = tp.output;
                    }
                    else
                    {
                        result.error = String(u8"could not run tint: ");
                        result.error += m_tint;
                    }
                    Cleanup(spvPath, wgslPath);
                    return result;
                }
            }

            Cleanup(spvPath, wgslPath);
            result.success = true;
            result.failedStage = WgslCookStage::Ok;
            return result;
        }

    private:
        [[nodiscard]] String ScratchPath(u64 id, StringView ext) const
        {
            String s(m_scratchDir);
            s += u8"/wgslcook_";
            char8_t digits[24];
            int n = 0;
            if (id == 0)
            {
                digits[n++] = u8'0';
            }
            else
            {
                char8_t tmp[24];
                int t = 0;
                while (id != 0 && t < 24)
                {
                    tmp[t++] = static_cast<char8_t>(u8'0' + (id % 10));
                    id /= 10;
                }
                while (t != 0)
                {
                    digits[n++] = tmp[--t];
                }
            }
            s += StringView(digits, static_cast<usize>(n));
            s += ext;
            return s;
        }

        static void Cleanup(const String& a, const String& b)
        {
            (void)FileDelete(a.AsView());
            (void)FileDelete(b.AsView());
        }

        Compiler* m_compiler;
        String m_scratchDir;
        String m_naga;
        String m_tint;
        bool m_validate = true;
        u64 m_counter = 0;
    };
}
