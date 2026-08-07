// Draconic Foundation - :path partition
//
// UTF-8 path string manipulation (POSIX '/' separator). Non-owning queries
// return StringView into the input; PathJoin builds a new String.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:path;

import :base;
import :allocator;
import :string;

export namespace draconic::foundation
{
    inline constexpr utf8char kPathSeparator = utf8char('/');

    [[nodiscard]] inline bool PathIsSeparator(utf8char c) noexcept
    {
        return c == utf8char('/') || c == utf8char('\\');
    }

    // Absolute = rooted on THIS host's filesystem. On Windows that also covers a
    // drive-qualified root ("D:\x", "D:/x") and UNC ("\\server\share"); a leading
    // separator alone ("\x") is drive-relative, but treating it as absolute keeps
    // the POSIX contract and matches how the engine builds paths. PathJoin depends
    // on this: an absolute `b` must win rather than be appended to `a`.
    [[nodiscard]] inline bool PathIsAbsolute(StringView path) noexcept
    {
        if (path.IsEmpty())
        {
            return false;
        }
        if (PathIsSeparator(path[0]))
        {
            return true; // POSIX root, and UNC "\\..." on Windows
        }
#if DRACONIC_PLATFORM_WINDOWS
        // "X:\..." / "X:/..." - a drive-qualified root.
        if (path.Size() >= 3 && path[1] == utf8char(':') && PathIsSeparator(path[2]))
        {
            const utf8char c = path[0];
            return (c >= utf8char('A') && c <= utf8char('Z')) ||
                   (c >= utf8char('a') && c <= utf8char('z'));
        }
#endif
        return false;
    }

    // The final component (after the last separator).
    [[nodiscard]] inline StringView PathFilename(StringView path) noexcept
    {
        usize start = 0;
        for (usize i = 0; i < path.Size(); ++i)
        {
            if (PathIsSeparator(path[i]))
            {
                start = i + 1;
            }
        }
        return path.SubStr(start, path.Size() - start);
    }

    // The extension including the dot (e.g. ".png"), or empty. A leading-dot
    // filename (".gitignore") has no extension.
    [[nodiscard]] inline StringView PathExtension(StringView path) noexcept
    {
        const StringView name = PathFilename(path);
        usize dot = name.Size();
        for (usize i = 0; i < name.Size(); ++i)
        {
            if (name[i] == utf8char('.'))
            {
                dot = i;
            }
        }
        if (dot == name.Size() || dot == 0)
        {
            return StringView{};
        }
        return name.SubStr(dot, name.Size() - dot);
    }

    // The filename without its extension.
    [[nodiscard]] inline StringView PathStem(StringView path) noexcept
    {
        const StringView name = PathFilename(path);
        const StringView ext = PathExtension(path);
        return name.SubStr(0, name.Size() - ext.Size());
    }

    // Everything before the last separator (empty if there is none).
    [[nodiscard]] inline StringView PathParent(StringView path) noexcept
    {
        usize lastSep = path.Size();
        for (usize i = 0; i < path.Size(); ++i)
        {
            if (PathIsSeparator(path[i]))
            {
                lastSep = i;
            }
        }
        if (lastSep == path.Size())
        {
            return StringView{};
        }
        return path.SubStr(0, lastSep);
    }

    // Joins two paths with a single separator. If `b` is absolute it wins.
    [[nodiscard]] inline String PathJoin(StringView a, StringView b,
                                         IAllocator& allocator = DefaultAllocator())
    {
        if (a.IsEmpty() || PathIsAbsolute(b))
        {
            return String{b, allocator};
        }

        String result{a, allocator};
        if (!PathIsSeparator(a[a.Size() - 1]))
        {
            result.PushBack(kPathSeparator);
        }
        result.Append(b);
        return result;
    }
}
