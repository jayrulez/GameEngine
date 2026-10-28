// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :iresource_provider partition
//
// Interface for loading external resources referenced by .sss stylesheets. The runtime layer
// provides an implementation that bridges to VFS. If no provider is given, @import and
// resource-loading factories (image, nine-slice, svg from file) fail gracefully. Ported from
// Sedulous.UI/src/Styling/Parser/IResourceProvider.bf.
//
// Divergence (language): pattern-B injected abstract base (no As*() query). Beef Result<void> ->
// bool (true = loaded); Result<IImageData> -> a borrowed const ImageData* (null = not found). The
// provider owns the returned image's lifetime.

module;
#include "Core/Prelude.h"

export module foundation.ui:iresource_provider;

import foundation.core;  // StringView, String
import foundation.image; // ImageData

using namespace foundation::core;
namespace image = foundation::image;

export namespace foundation::ui
{
    class IResourceProvider
    {
    public:
        virtual ~IResourceProvider() = default;

        /// Load text content from a path (for @import .sss files and @icon SVG files). Path is
        /// relative to the importing file or resource root. Returns true on success.
        virtual bool LoadText(StringView path, String& outText) = 0;

        /// Load image data from a path (for image() and nine-slice() factories). Returns a borrowed
        /// pointer owned by the provider, or null if not found.
        virtual const image::ImageData* LoadImage(StringView path) = 0;
    };
}
