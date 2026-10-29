// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Shaders.System - the `:file_provider` partition.
///
/// The DEV IShaderSourceProvider: engine built-in shaders as real files in a `Shaders`
/// folder of a filesystem the owner hands in (the application's data mount - the provider
/// never knows where on disk that is). Naming convention: the shader NAME is the file
/// stem, the stage is the double extension - `tonemap.ps.hlsl` serves
/// GetVariant("tonemap", Fragment, ...). Shared code lives in `.hlsli` next to them, served
/// to the compiler through the IShaderIncludeResolver half (so includes come from the
/// same mount - the compiler never touches the native filesystem).
///
/// The manifest is scanned EAGERLY (built-ins are enumerable - tooling wants the
/// list) but sources are read lazily. Hot reload goes through the mount's change
/// source when it has one: the folder is tracked once (the sweep is recursive), so
/// `.hlsli` edits are seen too - includers are unknown, so a `.hlsli` change reports
/// EVERY shader name (a full recompile is the correct dev answer). The stat sweep is
/// O(files) per Poll, so polls are throttled here, not in callers.

module;
#include "Core/Prelude.h"

export module foundation.shaders.system:file_provider;

import foundation.core;
import foundation.vfs;
import foundation.shaders;
import :shader_system;

namespace core = foundation::core;
namespace vfs = foundation::vfs;

export namespace foundation::shaders
{
    class FileShaderSourceProvider final : public IShaderSourceProvider,
                                           public IShaderIncludeResolver
    {
    public:
        // The allocator (required - the owner decides) backs the manifest and read buffers.
        explicit FileShaderSourceProvider(core::IAllocator& allocator) noexcept
            : m_allocator(&allocator), m_entries(allocator)
        {
        }

        /// PollChanges is called once per frame; only every Nth call actually stat-sweeps
        /// (the sweep is O(files)). ~1 second at 60 fps - dev hot reload, not a race.
        static constexpr core::u32 PollEveryNCalls = 60;

        /// Scans the manifest of `folder` inside `fileSystem` (both borrowed for the provider's
        /// lifetime; "" = the mount root). NotFound when the folder does not exist (callers fall
        /// back to registered strings, loudly); Unsupported when the mount cannot enumerate.
        core::Status Initialize(vfs::IFileSystem& fileSystem, core::StringView folder)
        {
            m_fileSystem = &fileSystem;
            m_folder = core::String(folder);
            m_entries.Clear();
            m_changes = nullptr;

            vfs::IEnumerableFileSystem* enumerable = fileSystem.AsEnumerable();
            if (enumerable == nullptr)
            {
                return core::ErrorCode::NotSupported;
            }
            if (!folder.IsEmpty() && !fileSystem.Exists(folder))
            {
                return core::ErrorCode::NotFound;
            }
            core::Array<vfs::DirEntry> entries;
            if (!enumerable->Enumerate(folder, entries).IsOk())
            {
                return core::ErrorCode::NotFound;
            }
            for (const vfs::DirEntry& entry : entries)
            {
                if (entry.isDirectory)
                {
                    continue; // flat folder; subdirectories are skipped
                }
                ShaderStage stage;
                core::StringView stem;
                if (!ParseFileName(entry.name.AsView(), stage, stem))
                {
                    continue; // .hlsli and unrelated files
                }
                Entry mapped;
                mapped.name = core::String(stem);
                mapped.stage = stage;
                mapped.path = Locate(entry.name.AsView());
                m_entries.PushBack(core::Move(mapped));
            }

            // Hot reload when the mount can watch (a native mount can; a pak cannot).
            if (vfs::IWatchableFileSystem* watchable = fileSystem.AsWatchable())
            {
                m_changes = watchable->ChangeSource();
                m_changes->Track(folder); // whole folder, recursive - catches .hlsli too
            }
            return core::ErrorCode::Ok;
        }

        /// The folder the manifest was scanned from (mount-relative; "" = the mount root).
        [[nodiscard]] core::StringView Folder() const noexcept { return m_folder.AsView(); }
        [[nodiscard]] core::usize ShaderFileCount() const noexcept { return m_entries.Size(); }
        /// True when the mount supports change polling (dev hot reload is live).
        [[nodiscard]] bool SupportsReload() const noexcept { return m_changes != nullptr; }

        // --- IShaderIncludeResolver ---
        // The preprocessor asks with the include path as written, relative to the includer: a
        // first-level include arrives bare ("common.hlsli") and is looked up in the folder;
        // a nested one already carries the folder prefix. Both are tried, folder-first.
        bool LoadInclude(core::StringView path, core::String& outSource) override
        {
            if (m_fileSystem == nullptr || path.IsEmpty())
            {
                return false;
            }
            const core::String inFolder = Locate(path);
            if (m_fileSystem->Exists(inFolder.AsView()) && ReadWholeFile(inFolder.AsView(), outSource))
            {
                return true;
            }
            return inFolder.AsView() != path && m_fileSystem->Exists(path) &&
                   ReadWholeFile(path, outSource);
        }

        bool FetchSource(core::StringView name, ShaderStage stage, core::String& outSource,
                         core::String& outEntryPoint) override
        {
            for (const Entry& entry : m_entries)
            {
                if (entry.stage == stage && entry.name.AsView() == name)
                {
                    outEntryPoint = core::String(kCorpusEntryPoint); // a file per stage
                    return ReadWholeFile(entry.path.AsView(), outSource);
                }
            }
            return false;
        }

        void CollectShaderNames(core::Array<core::String>& out) override
        {
            for (const Entry& entry : m_entries)
            {
                AppendUnique(out, entry.name.AsView());
            }
        }

        // Every .hlsli in the folder and its subfolders ("surface/forward.hlsli"), as an include
        // names it from the folder: LoadInclude serves each name back.
        void CollectIncludeNames(core::Array<core::String>& out) override
        {
            if (m_fileSystem != nullptr)
            {
                CollectIncludes(core::StringView{}, 0, out);
            }
        }

        bool PollChanges(core::Array<core::String>& outChangedNames) override
        {
            if (m_changes == nullptr)
            {
                return false;
            }
            core::Array<core::String> changedFiles;
            if (!ThrottleElapsed() || !m_changes->Poll(changedFiles))
            {
                return false;
            }
            bool any = false;
            for (const core::String& file : changedFiles)
            {
                if (EndsWith(file.AsView(), u8".hlsli"))
                {
                    // Includers are unknown; reload everything this provider serves.
                    for (const Entry& entry : m_entries)
                    {
                        any = true;
                        AppendUnique(outChangedNames, entry.name.AsView());
                    }
                    continue;
                }
                for (const Entry& entry : m_entries)
                {
                    if (entry.path.AsView() == file.AsView())
                    {
                        any = true;
                        AppendUnique(outChangedNames, entry.name.AsView());
                        break;
                    }
                }
            }
            return any;
        }

    private:
        struct Entry
        {
            core::String name; // shader name = file stem ("tonemap")
            ShaderStage stage; // from the double extension
            core::String path; // mount-relative ("Shaders/tonemap.ps.hlsl")
        };

        // Mount-relative path of a file inside the scanned folder.
        [[nodiscard]] core::String Locate(core::StringView fileName) const
        {
            if (m_folder.IsEmpty())
            {
                return core::String(fileName);
            }
            core::String path(m_folder.AsView());
            path.PushBack(core::utf8char('/'));
            path.Append(fileName);
            return path;
        }

        // The .hlsli under `relative` (folder-relative) and its subfolders, depth-limited.
        void CollectIncludes(core::StringView relative, core::u32 depth,
                             core::Array<core::String>& out) const
        {
            constexpr core::u32 kMaxFolderDepth = 8;
            vfs::IEnumerableFileSystem* enumerable = m_fileSystem->AsEnumerable();
            if (enumerable == nullptr)
            {
                return;
            }
            const core::String folder =
                relative.IsEmpty() ? core::String(m_folder.AsView()) : Locate(relative);
            core::Array<vfs::DirEntry> entries(*m_allocator);
            if (!enumerable->Enumerate(folder.AsView(), entries).IsOk())
            {
                return;
            }
            for (const vfs::DirEntry& entry : entries)
            {
                core::String path(relative);
                if (!relative.IsEmpty())
                {
                    path.PushBack(core::utf8char('/'));
                }
                path.Append(entry.name.AsView());
                if (entry.isDirectory)
                {
                    if (depth < kMaxFolderDepth)
                    {
                        CollectIncludes(path.AsView(), depth + 1, out);
                    }
                }
                else if (EndsWith(entry.name.AsView(), u8".hlsli"))
                {
                    out.PushBack(core::Move(path));
                }
            }
        }

        static bool EndsWith(core::StringView text, core::StringView suffix)
        {
            if (text.Size() < suffix.Size())
            {
                return false;
            }
            return text.SubStr(text.Size() - suffix.Size(), suffix.Size()) == suffix;
        }

        static void AppendUnique(core::Array<core::String>& out, core::StringView name)
        {
            for (const core::String& existing : out)
            {
                if (existing.AsView() == name)
                {
                    return;
                }
            }
            out.PushBack(core::String(name));
        }

        /// "tonemap.ps.hlsl" -> (Fragment, "tonemap"); false for anything else.
        static bool ParseFileName(core::StringView fileName, ShaderStage& outStage,
                                  core::StringView& outStem)
        {
            core::StringView suffix;
            if (EndsWith(fileName, u8".vs.hlsl"))
            {
                outStage = ShaderStage::Vertex;
                suffix = u8".vs.hlsl";
            }
            else if (EndsWith(fileName, u8".ps.hlsl"))
            {
                outStage = ShaderStage::Fragment;
                suffix = u8".ps.hlsl";
            }
            else if (EndsWith(fileName, u8".cs.hlsl"))
            {
                outStage = ShaderStage::Compute;
                suffix = u8".cs.hlsl";
            }
            else
            {
                return false;
            }
            outStem = fileName.SubStr(0, fileName.Size() - suffix.Size());
            return !outStem.IsEmpty();
        }

        bool ReadWholeFile(core::StringView path, core::String& outSource)
        {
            core::UniquePtr<core::IStream> stream = m_fileSystem->Open(path, core::FileMode::Read);
            if (!stream)
            {
                return false;
            }
            const core::i64 size = stream->Size();
            if (size < 0)
            {
                return false;
            }
            core::Array<core::u8> bytes(*m_allocator);
            bytes.Resize(static_cast<core::usize>(size));
            if (!bytes.IsEmpty() &&
                stream->Read(bytes.Data(), bytes.Size()) != static_cast<core::u64>(bytes.Size()))
            {
                return false;
            }
            outSource = core::String(core::StringView(
                reinterpret_cast<const core::utf8char*>(bytes.Data()), bytes.Size()));
            return true;
        }

        bool ThrottleElapsed()
        {
            if (++m_callsSinceSweep < PollEveryNCalls)
            {
                return false;
            }
            m_callsSinceSweep = 0;
            return true;
        }

        core::IAllocator* m_allocator;
        vfs::IFileSystem* m_fileSystem = nullptr; // borrowed (the application's data mount)
        core::String m_folder;                    // mount-relative folder ("Shaders")
        vfs::IChangeSource* m_changes = nullptr;  // owned by the mount; null = no reload
        core::Array<Entry> m_entries;
        core::u32 m_callsSinceSweep = 0;
    };
}
