// Draconic::VFS - :source_path partition.
//
// SourcePath: a typed MOUNT-RELATIVE logical path - the currency for source-file
// references in serialized asset data (editor::Asset::fileName and friends). One honest
// guarantee: whatever string it is constructed from, the stored form is forward-slash,
// relative, and dot-segment-free - so Windows-authored "Fonts\Roboto.ttf" heals into
// "Fonts/Roboto.ttf" instead of breaking every non-Windows VFS lookup.
//
// NOT a general OS path (see docs/design/path-type.md): no volumes, no macros, no
// absolute form. Strings that violate the contract (absolute, ".." escapes, a ':' scheme/
// volume) normalize to EMPTY - a missing reference, never a wrong one. Comparison is
// case-SENSITIVE on every platform (one rule everywhere; a lint catches Windows-authored
// case mismatches instead of a platform-dependent operator==).
//
// Wire shape: serializes as its string (ar.Text), IDENTICAL to the String fields it
// replaces - existing data loads unchanged, reads re-normalize on the way in.

module;
#include "Draconic.Core/Prelude.h"

export module draconic.vfs:source_path;

import draconic.core;

using namespace draconic::core;

export namespace draconic::vfs
{
    class SourcePath
    {
    public:
        SourcePath() = default;
        explicit SourcePath(StringView raw) : m_value(Normalize(raw)) {}

        [[nodiscard]] StringView View() const { return m_value.AsView(); }
        [[nodiscard]] bool IsEmpty() const { return m_value.IsEmpty(); }

        // "Fonts/Roboto.ttf" -> "ttf" (lowercased ASCII, no dot; "" when there is none).
        [[nodiscard]] String Extension() const
        {
            const StringView file = FileName();
            const usize dot = LastIndexOf(file, u8'.');
            if (dot == kNotFound || dot + 1 == file.Size())
            {
                return String();
            }
            String ext(file.SubStr(dot + 1, file.Size() - dot - 1));
            for (usize i = 0; i < ext.Size(); ++i)
            {
                if (ext[i] >= u8'A' && ext[i] <= u8'Z')
                {
                    ext[i] = static_cast<utf8char>(ext[i] - u8'A' + u8'a');
                }
            }
            return ext;
        }

        // "Fonts/Roboto.ttf" -> "Roboto.ttf".
        [[nodiscard]] StringView FileName() const
        {
            const StringView v = m_value.AsView();
            const usize slash = LastIndexOf(v, u8'/');
            return slash == kNotFound ? v : v.SubStr(slash + 1, v.Size() - slash - 1);
        }

        // "Fonts/Roboto.ttf" -> "Roboto".
        [[nodiscard]] StringView Stem() const
        {
            const StringView file = FileName();
            const usize dot = LastIndexOf(file, u8'.');
            return dot == kNotFound ? file : file.SubStr(0, dot);
        }

        // "Fonts/Roboto.ttf" -> "Fonts"; "Roboto.ttf" -> "".
        [[nodiscard]] StringView Directory() const
        {
            const StringView v = m_value.AsView();
            const usize slash = LastIndexOf(v, u8'/');
            return slash == kNotFound ? StringView() : v.SubStr(0, slash);
        }

        [[nodiscard]] bool operator==(const SourcePath& other) const
        {
            return m_value.AsView() == other.m_value.AsView();
        }
        [[nodiscard]] bool operator!=(const SourcePath& other) const { return !(*this == other); }
        [[nodiscard]] bool operator==(StringView other) const { return View() == other; }
        [[nodiscard]] bool operator!=(StringView other) const { return View() != other; }
        [[nodiscard]] bool operator<(const SourcePath& other) const
        {
            // Lexicographic byte compare, case-sensitive on every platform.
            const StringView a = m_value.AsView();
            const StringView b = other.m_value.AsView();
            const usize n = a.Size() < b.Size() ? a.Size() : b.Size();
            for (usize i = 0; i < n; ++i)
            {
                if (a[i] != b[i])
                {
                    return static_cast<u8>(a[i]) < static_cast<u8>(b[i]);
                }
            }
            return a.Size() < b.Size();
        }

        // The normalization contract, usable standalone (importers producing paths).
        // Invalid inputs (absolute, "..", ':') return the empty string.
        [[nodiscard]] static String Normalize(StringView raw)
        {
            String cleaned;
            cleaned.Reserve(raw.Size());
            for (usize i = 0; i < raw.Size(); ++i)
            {
                const utf8char c = raw[i];
                if (c == u8':')
                {
                    return String(); // scheme/volume - not a mount-relative path
                }
                cleaned.PushBack(c == u8'\\' ? u8'/' : c);
            }
            if (!cleaned.IsEmpty() && cleaned[0] == u8'/')
            {
                return String(); // absolute
            }

            // Segment-wise rebuild: drop "" (duplicate slashes) and "."; ".." rejects.
            String result;
            result.Reserve(cleaned.Size());
            usize start = 0;
            const StringView v = cleaned.AsView();
            while (start <= v.Size())
            {
                usize end = start;
                while (end < v.Size() && v[end] != u8'/')
                {
                    ++end;
                }
                const StringView segment = v.SubStr(start, end - start);
                if (segment.Size() == 2 && segment[0] == u8'.' && segment[1] == u8'.')
                {
                    return String(); // escapes the mount
                }
                const bool skip =
                    segment.IsEmpty() || (segment.Size() == 1 && segment[0] == u8'.');
                if (!skip)
                {
                    if (!result.IsEmpty())
                    {
                        result.PushBack(u8'/');
                    }
                    result += segment;
                }
                if (end == v.Size())
                {
                    break;
                }
                start = end + 1;
            }
            return result;
        }

    private:
        friend void Serialize(ISerializer& ar, SourcePath& path);

        static constexpr usize kNotFound = static_cast<usize>(-1);
        [[nodiscard]] static usize LastIndexOf(StringView v, utf8char c)
        {
            for (usize i = v.Size(); i > 0; --i)
            {
                if (v[i - 1] == c)
                {
                    return i - 1;
                }
            }
            return kNotFound;
        }

        String m_value;
    };

    // Wire-compatible with a plain String field (ar.Text); reads re-normalize, so
    // Windows-authored data heals on load without a version bump.
    inline void Serialize(ISerializer& ar, SourcePath& path)
    {
        ar.Text(path.m_value);
        if (ar.Mode() == SerializeMode::Read)
        {
            path.m_value = SourcePath::Normalize(path.m_value.AsView());
        }
    }

    // Reflects SourcePath as a value type (read accessors + a StringView constructor) for tooling
    // + scripting. The stored string is private, so there are no member properties - the accessors
    // ARE the surface (the accessor-gated-state convention). Idempotent; body in the impl unit
    // (gcc module-interface hygiene). Reflection track P1.
    void RegisterVFSReflection();
}
