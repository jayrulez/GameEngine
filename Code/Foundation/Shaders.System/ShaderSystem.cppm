// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Shaders.System - the `:shader_system` partition.
///
/// Compile-on-demand cache for shader variants. A shader is registered by name
/// per stage (its HLSL source); GetVariant(name, stage, flags) compiles the
/// permutation (flags become #defines) through DXC, creates the GPU ShaderModule, and
/// caches it by (nameHash, stage, flags). This is the layer above the stateless
/// foundation.shaders Compiler that the material and PSO layers build on. It needs the RHI
/// to create modules, so it is separate from the RHI-free foundation.shaders.

module;
#include "Core/Prelude.h"

export module foundation.shaders.system:shader_system;

import foundation.core;
import foundation.rhi;
import foundation.shaders;
namespace shaders = foundation::shaders;

namespace core = foundation::core;
namespace rhi = foundation::rhi;

export namespace foundation::shaders
{

    /// Where shader source is pulled from: instead of passes pushing strings, the
    /// ShaderSystem asks a provider on a source miss. In dev, files under the engine
    /// shader root (an edit hot-reloads with no C++ rebuild); in a dist, cooked bytecode
    /// packs. An explicit RegisterSource still wins, so modules with bespoke inline
    /// shaders (imgui) work unchanged.
    class IShaderSourceProvider
    {
    public:
        virtual ~IShaderSourceProvider() = default;

        /// HLSL text for (name, stage) and the entry point that stage's function is called in it;
        /// false when the provider has no such shader.
        virtual bool FetchSource(core::StringView name, ShaderStage stage, core::String& outSource,
                                 core::String& outEntryPoint) = 0;

        /// Every shader name the provider can serve (for tooling such as builtin dropdowns).
        virtual void CollectShaderNames(core::Array<core::String>& out) = 0;

        /// Every shared include (.hlsli) the provider serves, as an include names it
        /// ("wind.hlsli", "surface/forward.hlsli"). The shader system reads them through its
        /// include resolver for the effect blocks they declare.
        virtual void CollectIncludeNames(core::Array<core::String>& out) = 0;

        /// Appends names whose source changed since the last poll (dev file watch);
        /// returns true if any did. Called once per frame; implementations throttle.
        virtual bool PollChanges(core::Array<core::String>& outChangedNames) = 0;
    };

    // Map a device's expected shader format (rhi::ShaderFormat) to the cooked-pack blob format.
    // The two enums are parallel but distinct: rhi's belongs to the RHI layer, CookedShaderFormat
    // to the pack layer (foundation.shaders is RHI-free). Pure, so the mapping is unit-testable
    // without a live device.
    [[nodiscard]] inline CookedShaderFormat SelectCookedFormat(rhi::ShaderFormat format) noexcept
    {
        switch (format)
        {
        case rhi::ShaderFormat::DXIL:
            return CookedShaderFormat::Dxil;
        case rhi::ShaderFormat::WGSL:
            return CookedShaderFormat::Wgsl;
        case rhi::ShaderFormat::SpirV:
            break;
        }
        return CookedShaderFormat::SpirV;
    }

    // Compile-on-demand variant cache. The Compiler and Device are borrowed (owned by
    // the caller). Sources are registered per (name, stage), each with the entry point its
    // function is called (one file may serve both stages as VSMain and PSMain; the engine's own
    // shaders are a file per stage with `main`). Every module it creates records that entry
    // (rhi::ShaderModule::entryPoint), so a pipeline built on it need not name one.
    class ShaderSystem
    {
    public:
        ShaderSystem(Compiler& compiler, rhi::Device& device) noexcept
            : m_compiler(&compiler), m_device(&device)
        {
        }

        // Compiler-free construction for dist (pack) mode: GetVariant serves prebuilt blobs from the
        // cooked pack, so no DXC is needed. The on-demand compile path is unavailable (returns null).
        explicit ShaderSystem(rhi::Device& device) noexcept : m_compiler(nullptr), m_device(&device)
        {
        }

        ~ShaderSystem() { DestroyAll(); }

        ShaderSystem(const ShaderSystem&) = delete;
        ShaderSystem& operator=(const ShaderSystem&) = delete;

        // Register a shader's HLSL source for a stage (owned copy), and the entry point that
        // stage's function is called in it. With a declared variant mask (a cooked shader asset's
        // lattice) requests canonicalize onto it, as the corpus's do; without one the raw
        // requested flags compile (callers like the NormalMap-guarded tests rely on every
        // requested define being applied). Registering forgets the stage's failed variants.
        void RegisterSource(core::StringView name, ShaderStage stage, core::StringView hlsl,
                            core::StringView entryPoint,
                            core::Optional<ShaderFlags> declaredMask = {})
        {
            const core::u64 key = SourceKey(name, stage);
            m_sources.InsertOrAssign(key, Source{core::String(hlsl), core::String(entryPoint)});
            if (declaredMask.HasValue())
            {
                m_declaredMasks.InsertOrAssign(key, declaredMask.Value());
            }
            else
            {
                m_declaredMasks.Remove(key); // explicit registration overrides a provider fetch
            }
            ForgetFailures(ShaderNameHash(name));
        }

        // The source provider (borrowed; may be null; see IShaderSourceProvider).
        void SetSourceProvider(IShaderSourceProvider* provider)
        {
            m_provider = provider;
            m_effectsScanned = false;
        }
        [[nodiscard]] IShaderSourceProvider* SourceProvider() const noexcept
        {
            return m_provider;
        }

        // Dist mode: a cooked pack (borrowed) replaces on-demand compilation. When set, GetVariant
        // canonicalizes the request against the stage's declared mask and looks up the prebuilt
        // blob for this device's backend format, with no DXC and no source. A miss is a logged
        // cook-coverage bug. Set by the render subsystem when a shader pack ships beside the
        // executable.
        void SetCookedPack(const CookedShaderPack* pack)
        {
            m_pack = pack;
            m_effectsScanned = false;
        }
        [[nodiscard]] const CookedShaderPack* CookedPack() const noexcept { return m_pack; }

        // Dev hot reload: asks the provider which sources changed, drops their
        // cached source and variants, and bumps versions (the PSO cache rebuilds on
        // its existing poll). Call once per frame. Returns how many shaders reloaded.
        core::usize PumpReloads()
        {
            if (m_provider == nullptr)
            {
                return 0;
            }
            core::Array<core::String> changed;
            if (!m_provider->PollChanges(changed))
            {
                return 0;
            }
            for (const core::String& name : changed)
            {
                RemoveSource(name.AsView()); // refetched lazily on next GetVariant
                InvalidateShader(name.AsView());
            }
            // A registered shader (a project's) may include a changed library file; the provider
            // cannot say which, so every remembered failure gets another compile.
            m_failedVariants.Clear();
            // An include may have changed an effect block; read them again on the next ask.
            m_effectsScanned = false;
            return changed.Size();
        }

        // Whether a source is registered (or was fetched and cached) for `name`'s `stage`.
        [[nodiscard]] bool HasSource(core::StringView name, ShaderStage stage) const
        {
            return m_sources.Find(SourceKey(name, stage)) != nullptr;
        }

        // Drops the cached source text for every stage of `name` (the variant cache
        // is handled by InvalidateShader).
        void RemoveSource(core::StringView name)
        {
            constexpr ShaderStage kStages[] = {ShaderStage::Vertex, ShaderStage::Fragment,
                                               ShaderStage::Compute};
            for (const ShaderStage stage : kStages)
            {
                const core::u64 key = SourceKey(name, stage);
                m_sources.Remove(key);
                m_declaredMasks.Remove(key);
            }
        }

        // Where `#include`s resolve during on-demand compiles (borrowed; null means the
        // compiler's disk handler with no search path). The dev file provider is one: it serves
        // the shared .hlsli from the same mount the sources come from.
        void SetIncludeResolver(IShaderIncludeResolver* resolver)
        {
            m_includeResolver = resolver;
            m_effectsScanned = false;
        }
        [[nodiscard]] IShaderIncludeResolver* IncludeResolver() const noexcept
        {
            return m_includeResolver;
        }

        // The effects the engine's shaders declare (`effect` blocks in the corpus's includes):
        // the cooked pack's tables when a pack is set, else
        // parsed from the provider's includes, read through the include resolver, on the first
        // ask and again after a reload. What a material whose shader declares an effect's flag
        // takes its effect group from. The library, and pointers into it, stay valid until the
        // next reload, pack, provider or resolver change.
        [[nodiscard]] const ShaderEffectLibrary& Effects() const
        {
            if (m_pack != nullptr)
            {
                return m_pack->Effects();
            }
            if (!m_effectsScanned)
            {
                ScanSourceEffects();
            }
            return m_sourceEffects;
        }

        // An effect by its name (`Wind`); null when no include declares it.
        [[nodiscard]] const ShaderEffect* FindEffect(core::StringView name) const
        {
            return Effects().Find(name);
        }

        // The effect a variant flag turns on (one flag, `ShaderFlags::Wind`); null when none.
        [[nodiscard]] const ShaderEffect* EffectForFlag(ShaderFlags flag) const
        {
            return Effects().ForFlag(flag);
        }

        // The variant flags `name`'s `stage` declares (its `// variants` line, or the lattice a
        // cooked shader was registered or packed with): what a material on the shader may turn on,
        // so what effect groups it carries. Served the way
        // GetVariant would serve the stage: a registered source's mask, else the pack's, else the
        // provider's source, fetched and remembered as a compile would. None when unknown.
        [[nodiscard]] ShaderFlags DeclaredVariants(core::StringView name, ShaderStage stage)
        {
            const core::u64 srcKey = SourceKey(name, stage);
            const Source* registered = m_sources.Find(srcKey);
            const bool compiles = m_compiler != nullptr && registered != nullptr && !registered->hlsl.IsEmpty();
            if (m_pack != nullptr && !compiles)
            {
                return m_pack->DeclaredMask(ShaderNameHash(name), stage);
            }
            if (const ShaderFlags* mask = m_declaredMasks.Find(srcKey))
            {
                return *mask;
            }
            if (registered != nullptr)
            {
                // Registered without a lattice: the raw flags compile, so its directive says what
                // its code reads.
                const VariantDirective directive = ParseStageVariantDirective(registered->hlsl.AsView(), stage);
                return directive.present ? directive.mask : ShaderFlags::None;
            }
            if (m_provider != nullptr)
            {
                Source fetched;
                if (m_provider->FetchSource(name, stage, fetched.hlsl, fetched.entryPoint))
                {
                    // The corpus's rule, as GetCompiledVariant applies it.
                    const VariantDirective directive = ParseVariantDirective(fetched.hlsl.AsView());
                    const ShaderFlags mask = directive.present ? directive.mask : ShaderFlags::None;
                    m_sources.InsertOrAssign(srcKey, core::Move(fetched));
                    m_declaredMasks.InsertOrAssign(srcKey, mask);
                    return mask;
                }
            }
            return ShaderFlags::None;
        }

        // Get the GPU module for a variant, compiling and caching it on demand. Returns null if the
        // source is unknown or compilation fails. A failed variant is remembered until its source
        // changes (RegisterSource, InvalidateShader, a reload), so a broken shader is compiled
        // and reported once, not every frame the renderer asks for it.
        [[nodiscard]] rhi::ShaderModule* GetVariant(core::StringView name, ShaderStage stage,
                                                    ShaderFlags flags)
        {
            if (m_pack != nullptr)
            {
                // A registered source (a project's shader, a bespoke inline one) compiles while a
                // compiler is here: the editor's source is newer than any pack. Without one (a
                // dist), or when the source is empty (an export stripped it, and cooked the
                // shader into the pack instead: a dist's player can still carry a compiler, a
                // dev build does), the pack serves it like the corpus, with its entry points and
                // lattices.
                const Source* registered = m_sources.Find(SourceKey(name, stage));
                if (m_compiler != nullptr && registered != nullptr && !registered->hlsl.IsEmpty())
                {
                    return GetCompiledVariant(name, stage, flags);
                }
                return GetCookedVariant(name, stage, flags);
            }
            return GetCompiledVariant(name, stage, flags);
        }

        // Drop and destroy every cached variant of a shader and bump its version (the
        // reload signal consumers poll). Call on a shader reload. The next GetVariant
        // recompiles. Returns how many variants were invalidated.
        core::usize InvalidateShader(core::StringView name)
        {
            const core::u64 nameHash = ShaderNameHash(name);
            core::Array<ShaderVariantKey> toRemove;
            for (auto& e : m_cache)
            {
                if (e.key.nameHash == nameHash)
                {
                    if (e.value != nullptr)
                    {
                        m_device->DestroyShaderModule(e.value);
                    }
                    toRemove.PushBack(e.key);
                }
            }
            for (const ShaderVariantKey& k : toRemove)
            {
                m_cache.Remove(k);
            }
            ForgetFailures(nameHash);
            BumpVersion(nameHash);
            return toRemove.Size();
        }

        // Monotonic version of a shader: bumped by each InvalidateShader (that is, each
        // reload). The PSO cache stamps pipelines with this and rebuilds when it
        // changes. 0 if the shader was never registered or invalidated.
        [[nodiscard]] core::u64 Version(core::StringView name) noexcept
        {
            core::u64* v = m_versions.Find(ShaderNameHash(name));
            return (v != nullptr) ? *v : 0ull;
        }

    private:
        // The cooked-blob format this device consumes (see SelectCookedFormat): DXIL for DX12, WGSL
        // text for browser WebGPU, SPIR-V for everything else. The WebGPU backend ingests either
        // SPIR-V (native, through ShaderSourceSPIRV) or WGSL (browser); it sniffs the SPIR-V magic.
        [[nodiscard]] CookedShaderFormat FormatForDevice() const noexcept
        {
            return SelectCookedFormat(m_device->PreferredShaderFormat());
        }

        // Dev path: resolve the source (registered, or pulled from the provider) and canonicalize
        // provider-fetched (corpus) requests against the stage's declared variant mask as the
        // cooked path does. Dev and dist must canonicalize identically, and doing so dedupes
        // variants (a PS that ignores SKINNED is not recompiled per skin). Explicitly registered
        // sources have no mask entry and compile raw flags.
        [[nodiscard]] rhi::ShaderModule* GetCompiledVariant(core::StringView name,
                                                            ShaderStage stage, ShaderFlags flags)
        {
            const core::u64 srcKey = SourceKey(name, stage);
            Source* source = m_sources.Find(srcKey);
            if (source == nullptr && m_provider != nullptr)
            {
                Source fetched;
                if (m_provider->FetchSource(name, stage, fetched.hlsl, fetched.entryPoint))
                {
                    m_sources.InsertOrAssign(srcKey, core::Move(fetched));
                    source = m_sources.Find(srcKey);
                    // Corpus sources carry the variant directive; absent means single-variant
                    // (mask None), the same rule the cook applies.
                    const VariantDirective directive = ParseVariantDirective(source->hlsl.AsView());
                    m_declaredMasks.InsertOrAssign(
                        srcKey, directive.present ? directive.mask : ShaderFlags::None);
                }
            }
            if (source == nullptr)
            {
                return nullptr;
            }

            ShaderFlags canon = flags;
            if (const ShaderFlags* mask = m_declaredMasks.Find(srcKey))
            {
                canon = CanonicalizeFlags(flags, *mask);
            }
            const ShaderVariantKey key{ShaderNameHash(name), stage, canon};
            if (rhi::ShaderModule** cached = m_cache.Find(key))
            {
                return *cached;
            }
            if (m_failedVariants.Find(key) != nullptr)
            {
                return nullptr; // compiled and reported already; waits for the source to change
            }

            rhi::ShaderModule* module =
                Compile(source->hlsl.AsView(), source->entryPoint.AsView(), stage, canon);
            if (module == nullptr)
            {
                m_failedVariants.InsertOrAssign(key, true);
                return nullptr;
            }

            m_cache.InsertOrAssign(key, module);
            return module;
        }

        // Dist path: canonicalize against the declared mask, look up the prebuilt blob, and create
        // the GPU module directly. Cached under the canonical key so requests that differ only in
        // ignored flags dedupe. A miss is a cook-coverage bug (logged; returns null).
        [[nodiscard]] rhi::ShaderModule* GetCookedVariant(core::StringView name, ShaderStage stage,
                                                          ShaderFlags flags)
        {
            const core::u64 nameHash = ShaderNameHash(name);
            const ShaderFlags canon =
                CanonicalizeFlags(flags, m_pack->DeclaredMask(nameHash, stage));
            const ShaderVariantKey key{nameHash, stage, canon};
            if (rhi::ShaderModule** cached = m_cache.Find(key))
            {
                return *cached;
            }

            const CookedShaderFormat format = FormatForDevice();
            const core::Array<core::byte>* blob = m_pack->Find(nameHash, stage, canon, format);
            if (blob == nullptr)
            {
                // Logged once per variant: PSO layers retry every frame and the full
                // pack dump per frame would drown the log.
                const core::u64 missKey = (SourceKey(name, stage) * 1099511628211ull) ^
                                          (static_cast<core::u64>(canon) << 32);
                if (m_loggedFailures.Find(missKey) != nullptr)
                {
                    return nullptr;
                }
                m_loggedFailures.InsertOrAssign(missKey, true);
                const ShaderFlags mask = m_pack->DeclaredMask(nameHash, stage);
                rhi::LogErrorf("Cooked shader variant missing from the pack (a cook-coverage bug): "
                               "'%.*s' stage %u canon-flags %u (requested %u, declaredMask %u, "
                               "lookup format %u)",
                               static_cast<int>(name.Size()),
                               reinterpret_cast<const char*>(name.Data()),
                               static_cast<unsigned>(stage), static_cast<unsigned>(canon),
                               static_cast<unsigned>(flags), static_cast<unsigned>(mask),
                               static_cast<unsigned>(format));
                // Dump what the pack did cook for this (name, stage) so the gap shows its shape.
                m_pack->ForEachVariant(nameHash, stage,
                                       [&](ShaderFlags f, CookedShaderFormat fmt)
                                       {
                                           rhi::LogErrorf("    pack has: flags %u format %u",
                                                          static_cast<unsigned>(f),
                                                          static_cast<unsigned>(fmt));
                                       });
                return nullptr;
            }

            // The entry the cook compiled this stage for; a pack without one predates entry
            // names (format v3) and is refused at load, so empty here is a cook bug.
            const core::StringView entryPoint = m_pack->EntryPoint(nameHash, stage);
            if (entryPoint.IsEmpty())
            {
                rhi::LogErrorf("Cooked shader '%.*s' stage %u has no entry point in the pack",
                               static_cast<int>(name.Size()),
                               reinterpret_cast<const char*>(name.Data()),
                               static_cast<unsigned>(stage));
                return nullptr;
            }
            rhi::ShaderModuleDesc desc{};
            desc.code = core::Span<const core::u8>(reinterpret_cast<const core::u8*>(blob->Data()),
                                                   blob->Size());
            desc.entryPoint = entryPoint;
            rhi::ShaderModule* module = nullptr;
            if (m_device->CreateShaderModule(desc, module) != core::ErrorCode::Ok)
            {
                return nullptr;
            }
            m_cache.InsertOrAssign(key, module);
            return module;
        }

        [[nodiscard]] rhi::ShaderModule* Compile(core::StringView source,
                                                 core::StringView entryPoint, ShaderStage stage,
                                                 ShaderFlags flags)
        {
            if (m_compiler == nullptr)
            {
                return nullptr; // compiler-free (pack) mode: on-demand compilation is unavailable
            }
            const bool isDX12 = (m_device->type == rhi::DeviceType::DX12);
            const ShaderTarget target = isDX12 ? ShaderTarget::DXIL : ShaderTarget::SPIRV;

            core::Array<ShaderDefine> defines;
            AppendDefines(flags, defines);

            CompileOptions opts{};
            opts.shaderModel = u8"6_0";
            opts.optimizationLevel = 3;
            opts.defines = core::Span<const ShaderDefine>(defines.Data(), defines.Size());
            opts.includeResolver = m_includeResolver;
            opts.preserveInterface = ParsePreserveInterfaceDirective(source);
            if (!isDX12)
            {
                // Vulkan: shift register spaces so HLSL b/t/u/s registers don't collide
                // in SPIR-V (as the sample framework's CompileToModule does).
                opts.bindingShifts = shaders::BindingShifts::Standard();
                opts.bindingShiftSets = 4;
                if (m_device->type == rhi::DeviceType::WebGPU)
                {
                    opts.spirvTargetEnvironment = u8"vulkan1.1"; // naga rejects SPIR-V 1.4+
                }
            }

            CompileResult cr{};
            const core::Status r =
                m_compiler->compile(reinterpret_cast<const core::u8*>(source.Data()), source.Size(),
                                    stage, entryPoint, target, opts, cr);

            if (r != core::ErrorCode::Ok || !cr.success)
            {
                if (cr.messages != nullptr)
                {
                    rhi::LogErrorf("Shader variant compile failed: %s", cr.messages);
                }
                m_compiler->freeResult(cr);
                return nullptr;
            }

            rhi::ShaderModuleDesc desc{};
            desc.code = core::Span<const core::u8>(cr.bytecode, cr.bytecodeSize);
            desc.entryPoint = entryPoint;
            rhi::ShaderModule* module = nullptr;
            const core::Status mr = m_device->CreateShaderModule(desc, module);
            m_compiler->freeResult(cr);
            return (mr == core::ErrorCode::Ok) ? module : nullptr;
        }

        // Dev: parse the effect blocks of every include the provider serves. A malformed block
        // or a second effect for a name or flag is logged and left out (the compile that includes
        // it fails naming the line; the cook refuses it).
        void ScanSourceEffects() const
        {
            m_sourceEffects.Clear();
            m_effectsScanned = true;
            if (m_provider == nullptr || m_includeResolver == nullptr)
            {
                return;
            }
            core::Array<core::String> includes;
            m_provider->CollectIncludeNames(includes);
            for (const core::String& include : includes)
            {
                core::String text;
                if (!m_includeResolver->LoadInclude(include.AsView(), text) ||
                    !HasShaderEffect(text.AsView()))
                {
                    continue;
                }
                for (ShaderEffectBlock& block : ParseShaderEffects(text.AsView()))
                {
                    if (!block.IsOk())
                    {
                        for (const ShaderPropertyError& error : block.errors)
                        {
                            rhi::LogErrorf("Shader effect in '%.*s' line %u: %.*s",
                                           static_cast<int>(include.Size()),
                                           reinterpret_cast<const char*>(include.Data()),
                                           static_cast<unsigned>(error.line),
                                           static_cast<int>(error.message.Size()),
                                           reinterpret_cast<const char*>(error.message.Data()));
                        }
                        continue;
                    }
                    core::String error;
                    if (!m_sourceEffects.Add(core::Move(block.effect), error))
                    {
                        rhi::LogErrorf("Shader effect in '%.*s': %.*s",
                                       static_cast<int>(include.Size()),
                                       reinterpret_cast<const char*>(include.Data()),
                                       static_cast<int>(error.Size()),
                                       reinterpret_cast<const char*>(error.Data()));
                    }
                }
            }
        }

        void DestroyAll()
        {
            for (auto& e : m_cache)
            {
                if (e.value != nullptr)
                {
                    m_device->DestroyShaderModule(e.value);
                }
            }
            m_cache.Clear();
        }

        [[nodiscard]] static core::u64 SourceKey(core::StringView name, ShaderStage stage) noexcept
        {
            return (ShaderNameHash(name) * 1099511628211ull) ^ static_cast<core::u64>(stage);
        }

        // Drops the remembered failures of a shader's variants (its source changed).
        void ForgetFailures(core::u64 nameHash)
        {
            core::Array<ShaderVariantKey> forget;
            for (auto& e : m_failedVariants)
            {
                if (e.key.nameHash == nameHash)
                {
                    forget.PushBack(e.key);
                }
            }
            for (const ShaderVariantKey& k : forget)
            {
                m_failedVariants.Remove(k);
            }
        }

        void BumpVersion(core::u64 nameHash)
        {
            core::u64* v = m_versions.Find(nameHash);
            if (v == nullptr)
            {
                m_versions.InsertOrAssign(nameHash, 0ull);
                v = m_versions.Find(nameHash);
            }
            ++(*v);
        }

        // A stage's source and the name of its function in it.
        struct Source
        {
            core::String hlsl;
            core::String entryPoint;
        };

        Compiler* m_compiler;                             // borrowed
        rhi::Device* m_device;                            // borrowed
        IShaderSourceProvider* m_provider = nullptr;      // borrowed (may be null)
        const CookedShaderPack* m_pack = nullptr;         // dist mode: cooked blobs (borrowed)
        core::HashMap<core::u64, Source> m_sources;       // (name,stage) -> HLSL and entry
        // Declared variant mask per provider-fetched (corpus) source: the dev half of the
        // canonicalization contract. Explicitly registered sources have no entry (raw flags).
        core::HashMap<core::u64, ShaderFlags> m_declaredMasks;
        core::HashMap<core::u64, bool> m_loggedFailures; // once-per-key error throttling
        // Variants that compiled and failed (see GetVariant); forgotten when the source changes.
        core::HashMap<ShaderVariantKey, bool> m_failedVariants;
        core::HashMap<ShaderVariantKey, rhi::ShaderModule*>
            m_cache;                                    // variant -> GPU module (owned)
        core::HashMap<core::u64, core::u64> m_versions; // nameHash -> version
        IShaderIncludeResolver* m_includeResolver = nullptr; // borrowed
        // Dev: the effects parsed from the provider's includes (Effects()); read lazily from a
        // const accessor, so mutable.
        mutable ShaderEffectLibrary m_sourceEffects;
        mutable bool m_effectsScanned = false;
    };

} // namespace foundation::shaders
