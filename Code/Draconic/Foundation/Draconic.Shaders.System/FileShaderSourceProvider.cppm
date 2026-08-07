/// Draconic::ShaderSystem - the `:file_provider` partition.
///
/// The DEV IShaderSourceProvider: engine built-in shaders as real files under the
/// engine shader root (shaders.md P1). Naming convention: the shader NAME is the
/// file stem, the stage is the double extension - `tonemap.ps.hlsl` serves
/// GetVariant("tonemap", Fragment, ...). Shared code lives in `.hlsli` next to
/// them (the root doubles as the DXC include path).
///
/// The manifest is scanned EAGERLY (built-ins are enumerable - tooling wants the
/// list) but sources are read lazily. Hot reload goes through the VFS change
/// source: the whole mount is tracked once (the sweep is recursive), so `.hlsli`
/// edits are seen too - includers are unknown, so a `.hlsli` change reports EVERY
/// shader name (a full recompile is the correct dev answer). The stat sweep is
/// O(files) per Poll, so polls are throttled here, not in callers.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shaders.system:file_provider;

import draconic.foundation;
import draconic.vfs;
import draconic.shaders;
import :shader_system;

namespace foundation = draconic::foundation;
namespace vfs = draconic::vfs;

export namespace draconic::shaders
{
    class FileShaderSourceProvider final : public IShaderSourceProvider
    {
    public:
        /// PollChanges is called once per frame; only every Nth call actually stat-sweeps
        /// (the sweep is O(files)). ~1 second at 60 fps - dev hot reload, not a race.
        static constexpr foundation::u32 PollEveryNCalls = 60;

        /// Mounts `rootDirectory` and scans the manifest. NotFound when the root
        /// does not exist (callers fall back to registered strings, loudly).
        foundation::Status Initialize(foundation::StringView rootDirectory)
        {
            if (!foundation::DirectoryExists(rootDirectory))
            {
                return foundation::ErrorCode::NotFound;
            }
            m_root = foundation::String(rootDirectory);
            m_mount = foundation::MakeUnique<vfs::NativeFileSystem>(foundation::DefaultAllocator(),
                                                              rootDirectory);

            foundation::Array<vfs::DirEntry> entries;
            if (!m_mount->Enumerate(u8"", entries).IsOk())
            {
                return foundation::ErrorCode::Unknown;
            }
            for (const vfs::DirEntry& entry : entries)
            {
                if (entry.isDirectory)
                {
                    continue; // flat root for now; sub-trees can come with growth
                }
                ShaderStage stage;
                foundation::StringView stem;
                if (!ParseFileName(entry.name.AsView(), stage, stem))
                {
                    continue; // .hlsli and unrelated files
                }
                Entry mapped;
                mapped.name = foundation::String(stem);
                mapped.stage = stage;
                mapped.fileName = foundation::String(entry.name.AsView());
                m_entries.PushBack(foundation::Move(mapped));
            }

            m_changes = m_mount->AsWatchable()->ChangeSource();
            m_changes->Track(u8""); // whole mount, recursive - catches .hlsli too
            return foundation::ErrorCode::Ok;
        }

        [[nodiscard]] foundation::StringView RootDirectory() const noexcept { return m_root.AsView(); }
        [[nodiscard]] foundation::usize ShaderFileCount() const noexcept { return m_entries.Size(); }

        bool FetchSource(foundation::StringView name, ShaderStage stage,
                         foundation::String& outSource) override
        {
            for (const Entry& entry : m_entries)
            {
                if (entry.stage == stage && entry.name.AsView() == name)
                {
                    return ReadWholeFile(entry.fileName.AsView(), outSource);
                }
            }
            return false;
        }

        void CollectShaderNames(foundation::Array<foundation::String>& out) override
        {
            for (const Entry& entry : m_entries)
            {
                AppendUnique(out, entry.name.AsView());
            }
        }

        bool PollChanges(foundation::Array<foundation::String>& outChangedNames) override
        {
            if (m_changes == nullptr)
            {
                return false;
            }
            foundation::Array<foundation::String> changedFiles;
            if (!ThrottleElapsed() || !m_changes->Poll(changedFiles))
            {
                return false;
            }
            bool any = false;
            for (const foundation::String& file : changedFiles)
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
                    if (entry.fileName.AsView() == file.AsView())
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
            foundation::String name;     // shader name = file stem ("tonemap")
            ShaderStage stage;     // from the double extension
            foundation::String fileName; // mount-relative ("tonemap.ps.hlsl")
        };

        static bool EndsWith(foundation::StringView text, foundation::StringView suffix)
        {
            if (text.Size() < suffix.Size())
            {
                return false;
            }
            return text.SubStr(text.Size() - suffix.Size(), suffix.Size()) == suffix;
        }

        static void AppendUnique(foundation::Array<foundation::String>& out, foundation::StringView name)
        {
            for (const foundation::String& existing : out)
            {
                if (existing.AsView() == name)
                {
                    return;
                }
            }
            out.PushBack(foundation::String(name));
        }

        /// "tonemap.ps.hlsl" -> (Fragment, "tonemap"); false for anything else.
        static bool ParseFileName(foundation::StringView fileName, ShaderStage& outStage,
                                  foundation::StringView& outStem)
        {
            foundation::StringView suffix;
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

        bool ReadWholeFile(foundation::StringView fileName, foundation::String& outSource)
        {
            foundation::UniquePtr<foundation::IStream> stream =
                m_mount->Open(fileName, foundation::FileMode::Read);
            if (!stream)
            {
                return false;
            }
            const foundation::i64 size = stream->Size();
            if (size < 0)
            {
                return false;
            }
            foundation::Array<foundation::u8> bytes;
            bytes.Resize(static_cast<foundation::usize>(size));
            if (!bytes.IsEmpty() &&
                stream->Read(bytes.Data(), bytes.Size()) != static_cast<foundation::u64>(bytes.Size()))
            {
                return false;
            }
            outSource = foundation::String(foundation::StringView(
                reinterpret_cast<const foundation::utf8char*>(bytes.Data()), bytes.Size()));
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

        foundation::String m_root;
        foundation::UniquePtr<vfs::NativeFileSystem> m_mount;
        vfs::IChangeSource* m_changes = nullptr; // owned by the mount
        foundation::Array<Entry> m_entries;
        foundation::u32 m_callsSinceSweep = 0;
    };
}
