/// Draconic::ShaderSystem - the `:shader_system` partition.
///
/// Compile-on-demand + cache for shader VARIANTS. A shader is registered by name
/// per stage (its HLSL source); GetVariant(name, stage, flags) compiles the
/// permutation (flags -> #defines) via DXC, creates the GPU ShaderModule, and
/// caches it by (nameHash, stage, flags). This is the layer above the stateless
/// draconic.shaders Compiler that the material/PSO layers build on. Needs the RHI
/// to create modules, so it's separate from the RHI-free draconic.shaders.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shaders.system:shader_system;

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;

namespace foundation = draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::shaders
{

    /// The pull seam for shader SOURCE (shaders.md P1): instead of passes pushing
    /// strings, the ShaderSystem asks a provider on a source miss. Dev: files under
    /// the engine shader root (edit -> hot reload, zero C++ rebuild). Dist (P2):
    /// cooked bytecode packs. Explicit RegisterSource still wins - modules with
    /// bespoke inline shaders (imgui) keep working unchanged.
    class IShaderSourceProvider
    {
    public:
        virtual ~IShaderSourceProvider() = default;

        /// HLSL text for (name, stage); false when the provider has no such shader.
        virtual bool FetchSource(foundation::StringView name, ShaderStage stage,
                                 foundation::String& outSource) = 0;

        /// Every shader name the provider can serve (tooling: builtin dropdowns).
        virtual void CollectShaderNames(foundation::Array<foundation::String>& out) = 0;

        /// Appends names whose source changed since the last poll (dev file watch);
        /// returns true if any did. Called once per frame - implementations throttle.
        virtual bool PollChanges(foundation::Array<foundation::String>& outChangedNames) = 0;
    };

    // Map a device's expected shader format (rhi::ShaderFormat) to the cooked-pack blob format.
    // The two enums are parallel but distinct: rhi is RHI-layer, CookedShaderFormat is pack-layer
    // (draconic.shaders is RHI-free). Pure so the mapping is unit-testable without a live device.
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
    // the caller). Sources are registered per (name, stage) - vertex and fragment are
    // separate HLSL with `main` entry points (as in the Sedulous shader set).
    class ShaderSystem
    {
    public:
        ShaderSystem(Compiler& compiler, rhi::Device& device) noexcept
            : m_compiler(&compiler), m_device(&device)
        {
        }

        // Compiler-free construction for DIST/pack mode: GetVariant serves prebuilt blobs from the
        // cooked pack, so no DXC is needed. The on-demand compile path is unavailable (returns null).
        explicit ShaderSystem(rhi::Device& device) noexcept : m_compiler(nullptr), m_device(&device)
        {
        }

        ~ShaderSystem() { DestroyAll(); }

        ShaderSystem(const ShaderSystem&) = delete;
        ShaderSystem& operator=(const ShaderSystem&) = delete;

        // Register a shader's HLSL source for a stage (owned copy). Explicitly registered
        // sources are compiled with the RAW requested flags (no declared-mask model): they are
        // outside the corpus/cook lattice by definition, and callers like the NormalMap-guarded
        // tests rely on every requested define being applied.
        void RegisterSource(foundation::StringView name, ShaderStage stage, foundation::StringView hlsl)
        {
            const foundation::u64 key = SourceKey(name, stage);
            m_sources.InsertOrAssign(key, foundation::String(hlsl));
            m_declaredMasks.Remove(key); // explicit registration overrides a provider fetch
        }

        // The source-pull seam (borrowed; may be null - see IShaderSourceProvider).
        void SetSourceProvider(IShaderSourceProvider* provider) { m_provider = provider; }
        [[nodiscard]] IShaderSourceProvider* SourceProvider() const noexcept
        {
            return m_provider;
        }

        // Dist mode: a cooked pack (borrowed) replaces on-demand compilation. When set, GetVariant
        // canonicalizes the request against the stage's declared mask and looks up the prebuilt
        // blob for this device's backend format - no DXC, no source. A miss is a loud cook-coverage
        // bug. Set by the render subsystem when a shader pack ships beside the executable.
        void SetCookedPack(const CookedShaderPack* pack) { m_pack = pack; }
        [[nodiscard]] const CookedShaderPack* CookedPack() const noexcept { return m_pack; }

        // Dev hot reload: asks the provider which sources changed, drops their
        // cached source + variants, and bumps versions (the PSO cache rebuilds on
        // its existing poll). Call once per frame. Returns how many shaders reloaded.
        foundation::usize PumpReloads()
        {
            if (m_provider == nullptr)
            {
                return 0;
            }
            foundation::Array<foundation::String> changed;
            if (!m_provider->PollChanges(changed))
            {
                return 0;
            }
            for (const foundation::String& name : changed)
            {
                RemoveSource(name.AsView()); // refetched lazily on next GetVariant
                InvalidateShader(name.AsView());
            }
            return changed.Size();
        }

        // Drops the cached source text for every stage of `name` (the variant cache
        // is handled by InvalidateShader).
        void RemoveSource(foundation::StringView name)
        {
            constexpr ShaderStage kStages[] = {ShaderStage::Vertex, ShaderStage::Fragment,
                                               ShaderStage::Compute};
            for (const ShaderStage stage : kStages)
            {
                const foundation::u64 key = SourceKey(name, stage);
                m_sources.Remove(key);
                m_declaredMasks.Remove(key);
            }
        }

        // Include search paths for DXC #include resolution of shared .hlsli (owned).
        void SetIncludePaths(foundation::Span<const foundation::StringView> paths)
        {
            m_includePaths.Clear();
            for (foundation::usize i = 0; i < paths.Size(); ++i)
            {
                m_includePaths.PushBack(foundation::String(paths[i]));
            }
        }

        // Get (compile-on-demand + cache) the GPU module for a variant. Returns null
        // if the source is unknown or compilation fails (failures are NOT cached, so a
        // later request retries - e.g. after a fix).
        [[nodiscard]] rhi::ShaderModule* GetVariant(foundation::StringView name, ShaderStage stage,
                                                    ShaderFlags flags)
        {
            if (m_pack != nullptr)
            {
                // Explicit RegisterSource beats the pack: the engine pack covers only the
                // engine corpus, while registered sources carry bespoke inline shaders AND
                // user shader ASSETS (ShaderResource) - those must keep resolving in pack
                // mode or custom material shaders die in a dist.
                if (m_sources.Find(SourceKey(name, stage)) != nullptr)
                {
                    if (m_compiler != nullptr)
                    {
                        return GetCompiledVariant(name, stage, flags);
                    }
                    // Compiler-free dist: a registered SOURCE cannot be served. Loud once
                    // per (name, stage) - the fix is cooking user shaders to bytecode.
                    const foundation::u64 srcKey = SourceKey(name, stage);
                    if (m_loggedFailures.Find(srcKey) == nullptr)
                    {
                        m_loggedFailures.InsertOrAssign(srcKey, true);
                        rhi::LogErrorf("Registered shader source '%.*s' (stage %u) cannot be "
                                       "compiled in compiler-free pack mode - cook it or ship "
                                       "a compiler",
                                       static_cast<int>(name.Size()),
                                       reinterpret_cast<const char*>(name.Data()),
                                       static_cast<unsigned>(stage));
                    }
                    return nullptr;
                }
                return GetCookedVariant(name, stage, flags);
            }
            return GetCompiledVariant(name, stage, flags);
        }

        // Drop + destroy every cached variant of a shader and BUMP its version (the
        // reload signal consumers poll). Call on a shader reload. The next GetVariant
        // recompiles. Returns how many variants were invalidated.
        foundation::usize InvalidateShader(foundation::StringView name)
        {
            const foundation::u64 nameHash = ShaderNameHash(name);
            foundation::Array<ShaderVariantKey> toRemove;
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
            BumpVersion(nameHash);
            return toRemove.Size();
        }

        // Monotonic version of a shader: bumped each InvalidateShader (i.e. each
        // reload). The PSO cache stamps pipelines with this and rebuilds when it
        // changes. 0 if the shader was never registered/invalidated.
        [[nodiscard]] foundation::u64 Version(foundation::StringView name) noexcept
        {
            foundation::u64* v = m_versions.Find(ShaderNameHash(name));
            return (v != nullptr) ? *v : 0ull;
        }

    private:
        // The cooked-blob format this device consumes (see SelectCookedFormat): DX12 -> DXIL, browser
        // WebGPU -> WGSL text, everything else -> SPIR-V. The WebGPU backend ingests either SPIR-V
        // (native, via ShaderSourceSPIRV) or WGSL (browser) - it sniffs the SPIR-V magic.
        [[nodiscard]] CookedShaderFormat FormatForDevice() const noexcept
        {
            return SelectCookedFormat(m_device->PreferredShaderFormat());
        }

        // Dev path: resolve source (registered, or pulled from the provider), canonicalize
        // provider-fetched (corpus) requests against the stage's declared variant mask EXACTLY
        // like the cooked path does - the design requires dev and dist to canonicalize
        // identically, and it dedupes variants (a PS that ignores SKINNED stops recompiling
        // per skin). Explicitly registered sources have no mask entry and compile raw flags.
        [[nodiscard]] rhi::ShaderModule* GetCompiledVariant(foundation::StringView name,
                                                            ShaderStage stage, ShaderFlags flags)
        {
            const foundation::u64 srcKey = SourceKey(name, stage);
            foundation::String* source = m_sources.Find(srcKey);
            if (source == nullptr && m_provider != nullptr)
            {
                foundation::String fetched;
                if (m_provider->FetchSource(name, stage, fetched))
                {
                    m_sources.InsertOrAssign(srcKey, foundation::Move(fetched));
                    source = m_sources.Find(srcKey);
                    // Corpus sources carry the variant directive; absent means single-variant
                    // (mask None) - the same rule the cook applies.
                    const VariantDirective directive = ParseVariantDirective(source->AsView());
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

            rhi::ShaderModule* module = Compile(source->AsView(), stage, canon);
            if (module == nullptr)
            {
                return nullptr;
            }

            m_cache.InsertOrAssign(key, module);
            return module;
        }

        // Dist path: canonicalize against the declared mask, look up the prebuilt blob, and create
        // the GPU module directly. Cached under the CANONICAL key so requests that differ only in
        // ignored flags dedupe. A miss is a cook-coverage bug (loud, returns null).
        [[nodiscard]] rhi::ShaderModule* GetCookedVariant(foundation::StringView name, ShaderStage stage,
                                                          ShaderFlags flags)
        {
            const foundation::u64 nameHash = ShaderNameHash(name);
            const ShaderFlags canon =
                CanonicalizeFlags(flags, m_pack->DeclaredMask(nameHash, stage));
            const ShaderVariantKey key{nameHash, stage, canon};
            if (rhi::ShaderModule** cached = m_cache.Find(key))
            {
                return *cached;
            }

            const CookedShaderFormat format = FormatForDevice();
            const foundation::Array<foundation::byte>* blob = m_pack->Find(nameHash, stage, canon, format);
            if (blob == nullptr)
            {
                // Loud, but once per variant - PSO layers retry every frame and the full
                // pack dump per frame would drown the log.
                const foundation::u64 missKey = (SourceKey(name, stage) * 1099511628211ull) ^
                                          (static_cast<foundation::u64>(canon) << 32);
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
                // Dump what the pack DID cook for this (name, stage) so the gap shows its shape.
                m_pack->ForEachVariant(nameHash, stage,
                                       [&](ShaderFlags f, CookedShaderFormat fmt)
                                       {
                                           rhi::LogErrorf("    pack has: flags %u format %u",
                                                          static_cast<unsigned>(f),
                                                          static_cast<unsigned>(fmt));
                                       });
                return nullptr;
            }

            rhi::ShaderModuleDesc desc{};
            desc.code = foundation::Span<const foundation::u8>(reinterpret_cast<const foundation::u8*>(blob->Data()),
                                                   blob->Size());
            rhi::ShaderModule* module = nullptr;
            if (m_device->CreateShaderModule(desc, module) != foundation::ErrorCode::Ok)
            {
                return nullptr;
            }
            m_cache.InsertOrAssign(key, module);
            return module;
        }

        [[nodiscard]] rhi::ShaderModule* Compile(foundation::StringView source, ShaderStage stage,
                                                 ShaderFlags flags)
        {
            if (m_compiler == nullptr)
            {
                return nullptr; // compiler-free (pack) mode - on-demand compilation unavailable
            }
            const bool isDX12 = (m_device->type == rhi::DeviceType::DX12);
            const ShaderTarget target = isDX12 ? ShaderTarget::DXIL : ShaderTarget::SPIRV;

            foundation::Array<ShaderDefine> defines;
            AppendDefines(flags, defines);

            foundation::Array<foundation::StringView> includeViews;
            for (const foundation::String& p : m_includePaths)
            {
                includeViews.PushBack(p.AsView());
            }

            CompileOptions opts{};
            opts.shaderModel = u8"6_0";
            opts.optimizationLevel = 3;
            opts.defines = foundation::Span<const ShaderDefine>(defines.Data(), defines.Size());
            opts.includePaths =
                foundation::Span<const foundation::StringView>(includeViews.Data(), includeViews.Size());
            if (!isDX12)
            {
                // Vulkan: shift register spaces so HLSL b/t/u/s registers don't collide
                // in SPIR-V (matches the sample framework's CompileToModule).
                opts.bindingShifts = shaders::BindingShifts::Standard();
                opts.bindingShiftSets = 4;
                if (m_device->type == rhi::DeviceType::WebGPU)
                {
                    opts.spirvTargetEnvironment = u8"vulkan1.1"; // naga rejects SPIR-V 1.4+
                }
            }

            CompileResult cr{};
            const foundation::Status r =
                m_compiler->compile(reinterpret_cast<const foundation::u8*>(source.Data()), source.Size(),
                                    stage, u8"main", target, opts, cr);

            if (r != foundation::ErrorCode::Ok || !cr.success)
            {
                if (cr.messages != nullptr)
                {
                    rhi::LogErrorf("Shader variant compile failed: %s", cr.messages);
                }
                m_compiler->freeResult(cr);
                return nullptr;
            }

            rhi::ShaderModuleDesc desc{};
            desc.code = foundation::Span<const foundation::u8>(cr.bytecode, cr.bytecodeSize);
            rhi::ShaderModule* module = nullptr;
            const foundation::Status mr = m_device->CreateShaderModule(desc, module);
            m_compiler->freeResult(cr);
            return (mr == foundation::ErrorCode::Ok) ? module : nullptr;
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

        [[nodiscard]] static foundation::u64 SourceKey(foundation::StringView name, ShaderStage stage) noexcept
        {
            return (ShaderNameHash(name) * 1099511628211ull) ^ static_cast<foundation::u64>(stage);
        }

        void BumpVersion(foundation::u64 nameHash)
        {
            foundation::u64* v = m_versions.Find(nameHash);
            if (v == nullptr)
            {
                m_versions.InsertOrAssign(nameHash, 0ull);
                v = m_versions.Find(nameHash);
            }
            ++(*v);
        }

        Compiler* m_compiler;                             // borrowed
        rhi::Device* m_device;                            // borrowed
        IShaderSourceProvider* m_provider = nullptr;      // borrowed (may be null)
        const CookedShaderPack* m_pack = nullptr;         // dist mode: cooked blobs (borrowed)
        foundation::HashMap<foundation::u64, foundation::String> m_sources; // (name,stage) -> HLSL
        // Declared variant mask per PROVIDER-FETCHED (corpus) source - the dev half of the
        // canonicalization contract. Explicitly registered sources have no entry (raw flags).
        foundation::HashMap<foundation::u64, ShaderFlags> m_declaredMasks;
        foundation::HashMap<foundation::u64, bool> m_loggedFailures; // once-per-key error throttling
        foundation::HashMap<ShaderVariantKey, rhi::ShaderModule*>
            m_cache;                                    // variant -> GPU module (owned)
        foundation::HashMap<foundation::u64, foundation::u64> m_versions; // nameHash -> version
        foundation::Array<foundation::String> m_includePaths;
    };

} // namespace draconic::shaders
