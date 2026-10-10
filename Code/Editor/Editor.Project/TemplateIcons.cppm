// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :template_icons partition.
//
// An export template carries its own icon: an SVG file in its bundle (kTemplateIconFile, named by
// the manifest's `icon`), which the editor reads wherever it shows the template. Creating a
// template writes one in - the built-in icon its platform gets, one named (`handheld` for the
// Steam Deck build), or any .svg given - so a template made elsewhere shows what its maker chose.
// The built-in set, drawn for the engine (no third-party marks):
//   desktop  - a monitor on a stand          windows - the monitor, four panes on its screen
//   linux    - the monitor, a shell prompt   handheld - a wide body, screen between d-pad and buttons
//   web      - a globe                       phone   - a tall body, speaker slot and home bar

module;
#include "Core/Prelude.h"

export module editor.project:template_icons;

import foundation.core;
import foundation.vfs;

using namespace foundation::core;
namespace vfs = foundation::vfs;

export namespace editor
{
    /// The icon's file in a template bundle, the one creating a template writes.
    inline constexpr StringView kTemplateIconFile = u8"icon.svg";

    namespace template_icons_detail
    {
        // Windows desktop - a monitor on a stand, four panes on its screen.
        inline constexpr StringView kWindows = u8R"svg(<svg viewBox="0 0 24 24">
  <rect x="2.5" y="4" width="19" height="13" rx="1.5" fill="none" stroke="#E0E0E0" stroke-width="1.5"/>
  <path d="M9 21h6M12 17v4" fill="none" stroke="#E0E0E0" stroke-width="1.5" stroke-linecap="round"/>
  <rect x="7.5" y="7" width="4" height="3" fill="#E0E0E0"/>
  <rect x="12.5" y="7" width="4" height="3" fill="#E0E0E0"/>
  <rect x="7.5" y="11" width="4" height="3" fill="#E0E0E0"/>
  <rect x="12.5" y="11" width="4" height="3" fill="#E0E0E0"/>
</svg>)svg";

        // Linux desktop - a monitor on a stand, a shell prompt on its screen.
        inline constexpr StringView kLinux = u8R"svg(<svg viewBox="0 0 24 24">
  <rect x="2.5" y="4" width="19" height="13" rx="1.5" fill="none" stroke="#E0E0E0" stroke-width="1.5"/>
  <path d="M9 21h6M12 17v4" fill="none" stroke="#E0E0E0" stroke-width="1.5" stroke-linecap="round"/>
  <path d="M6.5 8l3 2.5-3 2.5" fill="none" stroke="#E0E0E0" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/>
  <path d="M11 13.5h5" fill="none" stroke="#E0E0E0" stroke-width="1.6" stroke-linecap="round"/>
</svg>)svg";

        // A desktop of no named OS - a monitor on a stand.
        inline constexpr StringView kDesktop = u8R"svg(<svg viewBox="0 0 24 24">
  <rect x="2.5" y="4" width="19" height="13" rx="1.5" fill="none" stroke="#E0E0E0" stroke-width="1.5"/>
  <path d="M9 21h6M12 17v4" fill="none" stroke="#E0E0E0" stroke-width="1.5" stroke-linecap="round"/>
</svg>)svg";

        // Handheld - a wide body, its screen between a d-pad and two buttons.
        inline constexpr StringView kHandheld = u8R"svg(<svg viewBox="0 0 24 24">
  <rect x="1.5" y="6.5" width="21" height="11" rx="3.5" fill="none" stroke="#E0E0E0" stroke-width="1.5"/>
  <rect x="7.5" y="8.5" width="9" height="7" rx="0.8" fill="none" stroke="#E0E0E0" stroke-width="1.2"/>
  <path d="M4.5 12h2M5.5 11v2" fill="none" stroke="#E0E0E0" stroke-width="1.2" stroke-linecap="round"/>
  <circle cx="18.6" cy="11" r="0.9" fill="#E0E0E0"/>
  <circle cx="19.6" cy="13.2" r="0.9" fill="#E0E0E0"/>
</svg>)svg";

        // Web - a globe: its outline, an equator and a meridian's two halves.
        inline constexpr StringView kWeb = u8R"svg(<svg viewBox="0 0 24 24">
  <circle cx="12" cy="12" r="9" fill="none" stroke="#E0E0E0" stroke-width="1.5"/>
  <path d="M3 12h18M12 3c-3 3.2-3 14.8 0 18M12 3c3 3.2 3 14.8 0 18" fill="none" stroke="#E0E0E0" stroke-width="1.3"/>
  <path d="M4.6 7.5h14.8M4.6 16.5h14.8" fill="none" stroke="#E0E0E0" stroke-width="1"/>
</svg>)svg";

        // Phone - a tall rounded body, a speaker slot and a home bar.
        inline constexpr StringView kPhone = u8R"svg(<svg viewBox="0 0 24 24">
  <rect x="6.5" y="2.5" width="11" height="19" rx="2.2" fill="none" stroke="#E0E0E0" stroke-width="1.5"/>
  <path d="M10.5 5h3M10 18.5h4" fill="none" stroke="#E0E0E0" stroke-width="1.3" stroke-linecap="round"/>
</svg>)svg";
    }

    struct BuiltInTemplateIcon
    {
        StringView name;
        StringView svg;
    };

    /// The built-in icons, by the names `Tools.Export --template create --icon` takes.
    inline constexpr BuiltInTemplateIcon kBuiltInTemplateIcons[] = {
        {u8"desktop", template_icons_detail::kDesktop},   {u8"windows", template_icons_detail::kWindows},
        {u8"linux", template_icons_detail::kLinux},       {u8"handheld", template_icons_detail::kHandheld},
        {u8"web", template_icons_detail::kWeb},           {u8"phone", template_icons_detail::kPhone},
    };

    /// A built-in icon's SVG by name; empty when there is no such icon.
    [[nodiscard]] inline StringView BuiltInTemplateIconSvg(StringView name) noexcept
    {
        for (const BuiltInTemplateIcon& icon : kBuiltInTemplateIcons)
        {
            if (icon.name == name)
            {
                return icon.svg;
            }
        }
        return {};
    }

    /// The built-in icon a template for `platform` gets when its maker names none.
    [[nodiscard]] inline StringView DefaultTemplateIconName(StringView platform) noexcept
    {
        if (platform == u8"Win64")
        {
            return u8"windows";
        }
        if (platform == u8"Linux64")
        {
            return u8"linux";
        }
        if (platform == u8"Web")
        {
            return u8"web";
        }
        return u8"desktop";
    }

    /// A text file's whole contents, or empty when it cannot be read: a template's icon.
    [[nodiscard]] inline String ReadTemplateFile(StringView directory, StringView file, IAllocator& allocator)
    {
        if (directory.IsEmpty() || file.IsEmpty())
        {
            return String(allocator);
        }
        vfs::NativeFileSystem fs(directory, allocator);
        UniquePtr<IStream> stream = fs.Open(file, FileMode::Read);
        if (!stream)
        {
            return String(allocator);
        }
        const usize size = static_cast<usize>(stream->Size());
        Array<byte> bytes(allocator);
        bytes.Resize(size);
        String text(allocator);
        if (size > 0 && stream->Read(bytes.Data(), size) == size)
        {
            text.Append(reinterpret_cast<const utf8char*>(bytes.Data()), size);
        }
        return text;
    }
}
