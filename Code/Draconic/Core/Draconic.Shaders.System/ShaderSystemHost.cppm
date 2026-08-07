/// Draconic::ShaderSystem - the `:host` partition.
///
/// ShaderSystemHost builds and owns a ready-to-use ShaderSystem for a device, encapsulating the
/// pack-vs-dev decision ONCE so every consumer (renderer, VG/UI, ImGui) resolves shaders the same
/// way via GetVariant():
///   - DEV mode: DXC + a FileShaderSourceProvider over the engine shader root => on-demand compile
///     with hot reload. The desktop dev path - PREFERRED whenever both a compiler and the source
///     root exist, so a stray cooked pack near the binaries can never silently freeze shaders
///     (hot reload is the P1 payoff; losing it must be a choice, not an accident).
///   - PACK mode: a cooked shaders.dpak beside the executable (or in the cwd) => no compiler,
///     prebuilt blobs in the device's backend format (WGSL in a browser). The dist / web path,
///     entered when dev mode is unavailable - or explicitly, via ShaderPackPolicy::ForcePack or
///     the DRACONIC_USE_SHADER_PACK environment variable (pack-on-desktop testing).
///
/// This is the single implementation of "how do I get a ShaderSystem for this device"; before it,
/// the renderer had its own copy and VG/UI/ImGui each DXC-compiled inline HLSL with no pack path.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shaders.system:host;

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import :shader_system;
import :file_provider;

using namespace draconic::foundation;

export namespace draconic::shaders
{
    /// How the host picks between the cooked pack and dev compilation. Automatic = dev when
    /// possible, pack otherwise (or when DRACONIC_USE_SHADER_PACK is set in the environment);
    /// the Force values pin one mode - primarily for tests and pack-on-desktop verification.
    enum class ShaderPackPolicy
    {
        Automatic,
        ForcePack,
        ForceDev,
    };

    class ShaderSystemHost
    {
    public:
        ShaderSystemHost() = default;
        ~ShaderSystemHost() { Shutdown(); }
        ShaderSystemHost(const ShaderSystemHost&) = delete;
        ShaderSystemHost& operator=(const ShaderSystemHost&) = delete;

        /// Build the ShaderSystem for `device`. `engineShaderRoot` is the dev-mode HLSL source root
        /// (e.g. DRACONIC_ENGINE_SHADER_DIR, or "Shaders" beside a dist). Returns true if a
        /// ShaderSystem is ready (either a dev compiler+provider or a cooked pack).
        bool Initialize(rhi::Device& device, StringView engineShaderRoot,
                        ShaderPackPolicy policy = ShaderPackPolicy::Automatic)
        {
            // DXC is OPTIONAL - only needed in dev mode. A dist/web build that ships a cooked pack
            // renders with no compiler at all.
            if (!createCompiler(CompilerDesc{}, m_compiler).IsOk())
            {
                m_compiler = nullptr;
            }

            StringView root = engineShaderRoot;
            if (!DirectoryExists(root) && DirectoryExists(u8"Shaders"))
            {
                root = u8"Shaders"; // relocated build - dist layout fallback
            }
            const bool devPossible = m_compiler != nullptr && DirectoryExists(root);

            // DEV FIRST: a present pack must not silently take over a working dev setup
            // (that kills hot reload with one log line). Pack mode is entered when dev is
            // impossible (dist/web) or explicitly requested (policy / environment).
            bool wantPack = !devPossible;
            if (policy == ShaderPackPolicy::ForcePack)
            {
                wantPack = true;
            }
            else if (policy == ShaderPackPolicy::ForceDev)
            {
                wantPack = false;
            }
            else if (devPossible && GetEnvironmentVariable(u8"DRACONIC_USE_SHADER_PACK").HasValue())
            {
                wantPack = true;
            }

            const bool havePack = wantPack && LoadPack();
            if (m_compiler == nullptr && !havePack)
            {
                return false; // neither a compiler nor a pack - nothing can resolve shaders
            }

            m_shaders = (m_compiler != nullptr)
                            ? MakeUnique<ShaderSystem>(DefaultAllocator(), *m_compiler, device)
                            : MakeUnique<ShaderSystem>(DefaultAllocator(), device);

            if (havePack)
            {
                m_shaders->SetCookedPack(m_pack.Get());
                rhi::LogInfof("ShaderSystemHost: cooked shader pack (%u variants)%s",
                              static_cast<unsigned>(m_pack->Count()),
                              m_compiler != nullptr ? " - registered sources still compile" : "");
            }
            else
            {
                if (wantPack)
                {
                    rhi::LogErrorf("ShaderSystemHost: pack mode requested but no usable "
                                   "shaders.dpak found - falling back to dev compilation");
                }
                m_provider = MakeUnique<FileShaderSourceProvider>(DefaultAllocator());
                if (m_provider->Initialize(root).IsOk())
                {
                    m_shaders->SetSourceProvider(m_provider.Get());
                    const StringView includePaths[] = {m_provider->RootDirectory()};
                    m_shaders->SetIncludePaths(Span<const StringView>{includePaths, 1});
                }
                else
                {
                    m_provider.Reset(); // no source root: only explicit RegisterSource works
                    rhi::LogErrorf("ShaderSystemHost: engine shader root not found (%.*s) - only "
                                   "explicitly registered shaders will resolve",
                                   static_cast<int>(root.Size()),
                                   reinterpret_cast<const char*>(root.Data()));
                }
            }
            return true;
        }

        [[nodiscard]] ShaderSystem* System() noexcept { return m_shaders.Get(); }
        [[nodiscard]] bool IsReady() const noexcept { return m_shaders.Get() != nullptr; }
        /// True when serving prebuilt blobs (dist/web) rather than compiling on demand (dev).
        [[nodiscard]] bool UsingPack() const noexcept { return m_pack.Get() != nullptr; }
        /// Cooked-variant count when in pack mode (0 in dev mode) - for diagnostics.
        [[nodiscard]] u32 PackVariantCount() const noexcept
        {
            return m_pack.Get() != nullptr ? m_pack->Count() : 0u;
        }

        /// Resolve a shader variant to a GPU module (null if not ready / unknown). Delegates to the
        /// owned ShaderSystem - pack lookup or dev compile-and-cache.
        [[nodiscard]] rhi::ShaderModule* GetVariant(StringView name, ShaderStage stage,
                                                    ShaderFlags flags)
        {
            return m_shaders.Get() != nullptr ? m_shaders->GetVariant(name, stage, flags) : nullptr;
        }

        void Shutdown()
        {
            m_shaders.Reset(); // destroys cached modules first
            m_provider.Reset();
            m_pack.Reset();
            if (m_compiler != nullptr)
            {
                m_compiler->Destroy();
                m_compiler = nullptr;
            }
        }

    private:
        // Look for shaders.dpak beside the executable (robust for a relocated dist), then the cwd.
        bool LoadPack()
        {
            constexpr StringView kPackFile = u8"shaders.dpak";
            String paths[2];
            const String exeDir = GetExecutableDirectory();
            if (!exeDir.IsEmpty())
            {
                paths[0] = exeDir;
                paths[0] += u8"/";
                paths[0] += kPackFile;
            }
            paths[1] = String(kPackFile);

            for (const String& path : paths)
            {
                if (path.IsEmpty() || !FileExists(path.AsView()))
                {
                    continue;
                }
                FileStream file(path.AsView(), FileMode::Read);
                if (!file.IsValid())
                {
                    continue;
                }
                UniquePtr<CookedShaderPack> pack = MakeUnique<CookedShaderPack>(DefaultAllocator());
                if (pack->Read(file).IsOk() && !pack->IsEmpty())
                {
                    m_pack = Move(pack);
                    return true;
                }
            }
            return false;
        }

        Compiler* m_compiler = nullptr; // owned (Destroy()); null in pack/dist mode
        UniquePtr<CookedShaderPack> m_pack;
        UniquePtr<FileShaderSourceProvider> m_provider;
        UniquePtr<ShaderSystem> m_shaders;
    };
}
