// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :resource_provider partition
//
// IResourceProvider: the seam the CSS engine uses to load image assets referenced by
// background-image: url(path). Aligned with Sedulous.UI / foundation.ui's IResourceProvider - it
// returns a raw, provider-owned image (not a framework Drawable), so the provider stays a pure
// asset loader (VFS + foundation.image) with no dependency on the GUI's drawable types; the
// StyleApplier wraps the image into an ImageDrawable. Font resolution is a separate seam
// (IFontProvider). @import / SVG text loading (Sedulous's LoadText) is not implemented.

module;
#include "Core/Prelude.h"

export module experimental.gui:resource_provider;

import foundation.core;  // StringView
import foundation.image; // ImageData
import foundation.vg.svg; // SVGDocument

using namespace foundation::core;
namespace core = foundation::core;
namespace image = foundation::image;

export namespace experimental::gui
{
    class IResourceProvider
    {
    public:
        virtual ~IResourceProvider() = default;

        // Load image data for a path (background-image: url(path)). Returns a borrowed pointer
        // owned by the provider (valid until the provider releases it), or null if not found.
        [[nodiscard]] virtual const image::ImageData* LoadImage(core::StringView path) = 0;
        // A text file's contents (a sheet's @import, E13 of Specs/gui-lab-evolution-2.md): true and
        // `out` filled when it is there.
        [[nodiscard]] virtual bool LoadText(core::StringView path, core::String& out) = 0;
        // An SVG picture (a CSS image whose path ends in .svg, E13): a borrowed document the provider
        // keeps (parsed once), drawn as paths at whatever size it fills; null if not found.
        [[nodiscard]] virtual const foundation::vg::svg::SVGDocument* LoadSvg(core::StringView path) = 0;
    };

    /// Whether a picture's path names an SVG (by its ending, either case): drawn as paths, not
    /// loaded as a raster.
    [[nodiscard]] inline bool IsSvgPath(core::StringView path) noexcept
    {
        if (path.Size() <= 4)
            return false;
        const core::StringView end = path.SubStr(path.Size() - 4, 4);
        return end == core::StringView(u8".svg") || end == core::StringView(u8".SVG");
    }
}
