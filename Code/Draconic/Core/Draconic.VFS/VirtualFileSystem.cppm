// Draconic::VFS - :vfs partition
//
// VirtualFileSystem: a scheme mount table. Paths are `scheme://locator`; the
// scheme selects a mounted backend and the locator (mount-relative, no scheme)
// is forwarded to it. It is a router, so it does not itself advertise
// capabilities - resolve a specific mount via GetMount(scheme) and query that
// mount's As*() instead. Mounts are non-owning.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vfs:vfs;

import draconic.foundation;
import :ifilesystem;

using namespace draconic::foundation;

export namespace draconic::vfs
{
    class VirtualFileSystem final : public IFileSystem
    {
    public:
        // Mounts a backend under a scheme, e.g. Mount(u8"project", fs) routes
        // "project://...". Non-owning; the backend must outlive this.
        void Mount(StringView scheme, IFileSystem& backend)
        {
            m_mounts.PushBack(MountPoint{String(scheme), &backend});
        }

        // Resolves the backend registered for a scheme, or null.
        [[nodiscard]] IFileSystem* GetMount(StringView scheme)
        {
            for (MountPoint& mount : m_mounts)
            {
                if (mount.scheme.AsView() == scheme)
                {
                    return mount.backend;
                }
            }
            return nullptr;
        }

        [[nodiscard]] UniquePtr<IStream> Open(StringView path, FileMode mode) override
        {
            StringView scheme;
            StringView locator;
            if (!SplitScheme(path, scheme, locator))
            {
                return UniquePtr<IStream>{};
            }
            IFileSystem* backend = GetMount(scheme);
            return backend != nullptr ? backend->Open(locator, mode) : UniquePtr<IStream>{};
        }

        [[nodiscard]] bool Exists(StringView path) override
        {
            StringView scheme;
            StringView locator;
            if (!SplitScheme(path, scheme, locator))
            {
                return false;
            }
            IFileSystem* backend = GetMount(scheme);
            return backend != nullptr && backend->Exists(locator);
        }

    private:
        struct MountPoint
        {
            String scheme;
            IFileSystem* backend;
        };

        // Splits "scheme://locator" into its parts. Returns false if there is no
        // "://" separator (schemeless paths are rejected).
        [[nodiscard]] static bool SplitScheme(StringView path, StringView& outScheme,
                                              StringView& outLocator)
        {
            constexpr StringView sep = u8"://";
            for (usize i = 0; i + sep.Size() <= path.Size(); ++i)
            {
                if (path.SubStr(i, sep.Size()) == sep)
                {
                    outScheme = path.SubStr(0, i);
                    outLocator = path.SubStr(i + sep.Size(), path.Size() - i - sep.Size());
                    return true;
                }
            }
            return false;
        }

        Array<MountPoint> m_mounts;
    };
}
