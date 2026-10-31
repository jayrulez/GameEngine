// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Shaders.System - the `:host` partition.
///
/// ShaderSystemHost builds and owns a ready-to-use ShaderSystem for a device. It makes the
/// choice between the pack and dev compilation in one place, so every consumer (renderer,
/// VG/UI, ImGui) resolves shaders the same way through GetVariant(). Both modes read from the
/// data filesystem the application hands in (its mounted data root; see foundation.vfs
/// ResolveDataRoot); the host knows the layout under it, never where it is:
///   - Dev mode: DXC and a FileShaderSourceProvider over `Engine/Shaders/`, compiling on demand
///     with hot reload (includes resolve through the same mount). This is the desktop dev path,
///     preferred whenever both a compiler and the source folder exist, so a stray cooked pack
///     can never silently freeze shaders: losing hot reload must be a choice, not an accident.
///   - Pack mode: the cooked `Engine/Shaders/shaders.dpak`, with no compiler and prebuilt blobs
///     in the device's backend format (WGSL in a browser). The dist and web path, entered when
///     dev mode is unavailable, or explicitly through ShaderPackPolicy::ForcePack, the
///     OPTION_USE_SHADER_PACK environment variable (pack-on-desktop testing), or an
///     `Engine/Shaders/.pack-first` marker in the data root. An editor distribution ships the
///     sources (to cook an export's pack) beside its cooked pack and starts from the pack; the
///     dev tree never has the marker, so its hot reload stays.

module;
#include "Core/Prelude.h"

export module foundation.shaders.system:host;

import foundation.core;
import foundation.rhi;
import foundation.vfs;
import foundation.shaders;
import :shader_system;
import :file_provider;

using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace vfs = foundation::vfs;

export namespace foundation::shaders
{
    /// How the host picks between the cooked pack and dev compilation. Automatic = dev when
    /// possible, pack otherwise (or when OPTION_USE_SHADER_PACK is set in the environment);
    /// the Force values pin one mode, mainly for tests and pack-on-desktop verification.
    enum class ShaderPackPolicy
    {
        Automatic,
        ForcePack,
        ForceDev,
    };

    /// The engine shader layout under the data root, the one place it is spelled. The cook
    /// tools and the export stage write to the same names.
    inline constexpr StringView kShaderFolder = u8"Engine/Shaders";
    inline constexpr StringView kShaderPackFile = u8"shaders.dpak";
    inline constexpr StringView kShaderPackPath = u8"Engine/Shaders/shaders.dpak";
    // A data root that ships sources and a pack and wants the pack (an editor distribution).
    inline constexpr StringView kShaderPackFirstMarker = u8"Engine/Shaders/.pack-first";

    class ShaderSystemHost
    {
    public:
        // The allocator (required; the owner decides) backs the shader system,
        // provider, and cooked packs.
        explicit ShaderSystemHost(core::IAllocator& allocator) noexcept : m_allocator(&allocator)
        {
        }
        ~ShaderSystemHost() { Shutdown(); }
        ShaderSystemHost(const ShaderSystemHost&) = delete;
        ShaderSystemHost& operator=(const ShaderSystemHost&) = delete;

        /// Build the ShaderSystem for `device` from `dataFileSystem` (the application's data
        /// mount, borrowed for the host's lifetime): dev sources in `Engine/Shaders/`, or the
        /// cooked `Engine/Shaders/shaders.dpak`. Returns true if a ShaderSystem is ready (either
        /// a dev compiler and provider or a cooked pack).
        bool Initialize(rhi::Device& device, vfs::IFileSystem& dataFileSystem,
                        ShaderPackPolicy policy = ShaderPackPolicy::Automatic)
        {
            m_dataFileSystem = &dataFileSystem;
            // DXC is optional, needed only in dev mode. A dist or web build that ships a cooked pack
            // renders with no compiler at all.
            if (!createCompiler(CompilerDesc{}, m_compiler).IsOk())
            {
                m_compiler = nullptr;
            }

            // "Sources present" means stage files, not the folder: a dist stages only the cooked
            // pack under Engine/Shaders/, and an editor dist ships DXC too, so judging by the
            // folder alone would put that dist in dev mode over an empty corpus.
            const bool haveSources = HasShaderSources(dataFileSystem);
            const bool devPossible = m_compiler != nullptr && haveSources;

            // Dev first: a present pack must not silently take over a working dev setup
            // (that would lose hot reload with one log line). Pack mode is entered when dev is
            // impossible (dist or web) or explicitly requested (policy or environment).
            bool wantPack = !devPossible;
            if (policy == ShaderPackPolicy::ForcePack)
            {
                wantPack = true;
            }
            else if (policy == ShaderPackPolicy::ForceDev)
            {
                wantPack = false;
            }
            else if (devPossible && (GetEnvironmentVariable(u8"OPTION_USE_SHADER_PACK").HasValue() ||
                                     dataFileSystem.Exists(kShaderPackFirstMarker)))
            {
                wantPack = true; // asked for: testing, or a distribution that marks itself
            }

            const bool havePack = wantPack && LoadPack();
            if (m_compiler == nullptr && !havePack)
            {
                return false; // neither a compiler nor a pack: nothing can resolve shaders
            }

            m_shaders = (m_compiler != nullptr)
                            ? MakeUnique<ShaderSystem>(*m_allocator, *m_compiler, device)
                            : MakeUnique<ShaderSystem>(*m_allocator, device);

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
                                   "Engine/Shaders/shaders.dpak in the data root - falling back to dev "
                                   "compilation");
                }
                m_provider = MakeUnique<FileShaderSourceProvider>(*m_allocator, *m_allocator);
                if (m_provider->Initialize(dataFileSystem, kShaderFolder).IsOk())
                {
                    m_shaders->SetSourceProvider(m_provider.Get());
                    m_shaders->SetIncludeResolver(m_provider.Get());
                }
                else
                {
                    m_provider.Reset(); // no source folder: only explicit RegisterSource works
                    rhi::LogErrorf("ShaderSystemHost: no Shaders/ folder in the data root - only "
                                   "explicitly registered shaders will resolve");
                }
            }
            return true;
        }

        [[nodiscard]] ShaderSystem* System() noexcept { return m_shaders.Get(); }
        [[nodiscard]] bool IsReady() const noexcept { return m_shaders.Get() != nullptr; }
        /// True when serving prebuilt blobs (dist or web) rather than compiling on demand (dev).
        [[nodiscard]] bool UsingPack() const noexcept { return m_pack.Get() != nullptr; }
        /// Cooked-variant count in pack mode (0 in dev mode), for diagnostics.
        [[nodiscard]] u32 PackVariantCount() const noexcept
        {
            return m_pack.Get() != nullptr ? static_cast<u32>(m_pack->Count()) : 0u;
        }

        /// Resolve a shader variant to a GPU module (null if not ready or unknown). Delegates to the
        /// owned ShaderSystem: a pack lookup or a dev compile-and-cache.
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
        // True when Engine/Shaders/ holds at least one .hlsl stage file or .hlsli include (the dev
        // corpus); false for a missing folder or one holding only the cooked pack.
        [[nodiscard]] static bool HasShaderSources(vfs::IFileSystem& dataFileSystem)
        {
            if (!dataFileSystem.Exists(kShaderFolder))
            {
                return false;
            }
            vfs::IEnumerableFileSystem* enumerable = dataFileSystem.AsEnumerable();
            if (enumerable == nullptr)
            {
                return true; // a mount that cannot list: the folder is the only evidence
            }
            Array<vfs::DirEntry> entries;
            if (!enumerable->Enumerate(kShaderFolder, entries).IsOk())
            {
                return false;
            }
            for (const vfs::DirEntry& entry : entries)
            {
                if (entry.isDirectory)
                {
                    continue;
                }
                const StringView name = entry.name.AsView();
                if (name.EndsWith(u8".hlsl") || name.EndsWith(u8".hlsli"))
                {
                    return true;
                }
            }
            return false;
        }

        // The cooked pack at its one location under the data root.
        bool LoadPack()
        {
            UniquePtr<IStream> file = m_dataFileSystem->Open(kShaderPackPath, FileMode::Read);
            if (!file || !file->IsValid())
            {
                return false;
            }
            UniquePtr<CookedShaderPack> pack = MakeUnique<CookedShaderPack>(*m_allocator);
            if (pack->Read(*file).IsOk() && !pack->IsEmpty())
            {
                m_pack = Move(pack);
                return true;
            }
            return false;
        }

        Compiler* m_compiler = nullptr; // owned (Destroy()); null in pack (dist) mode
        UniquePtr<CookedShaderPack> m_pack;
        UniquePtr<FileShaderSourceProvider> m_provider;
        core::IAllocator* m_allocator;
        vfs::IFileSystem* m_dataFileSystem = nullptr; // borrowed (the application's data mount)
        UniquePtr<ShaderSystem> m_shaders;
    };
}
