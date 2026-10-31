// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :picture_baker partition
//
// IPictureBaker: the interface through which the GUI asks for a vector picture to be drawn once,
// at the size it shows in device pixels, and kept as a texture. A picture that would otherwise
// tessellate its paths every frame (an SVG skin's nine pieces, each the whole document clipped to
// its place) then draws as textured quads instead. The host's renderer does the baking, into a GPU
// texture that is never read back; the GUI only asks. A picture that is asked for but not yet baked
// comes back null and is queued. The GUI draws it as paths meanwhile, and redraws when the baker's
// generation changes, which means a bake has finished.

module;
#include "Core/Prelude.h"

export module foundation.gui:picture_baker;

import foundation.core;
import foundation.image;
import foundation.vg.svg;

using namespace foundation::core;

export namespace foundation::gui
{
    /// Where an atlas keeps a picture: the page the renderer samples (premultiplied colour, as a
    /// bake's texture) and the picture's rectangle on it, in the page's pixels.
    struct PictureRegion
    {
        const foundation::image::ImageData* Page = nullptr;
        Rectangle Source{};
    };

    /// IPictureBaker::kLargestAtlasPicture, kept at namespace scope so another library can read it:
    /// MSVC does not export a class's static constexpr member from a shared library.
    inline constexpr u32 kIPictureBakerLargestAtlasPicture = 128;

    class IPictureBaker
    {
    public:
        virtual ~IPictureBaker() = default;

        /// The largest side a picture may have in an atlas; larger pictures are baked alone (Baked).
        static constexpr u32 kLargestAtlasPicture = kIPictureBakerLargestAtlasPicture;

        /// Returns where `document`, at `width` by `height` device pixels, sits in an atlas shared by
        /// many pictures (such as icons), so pictures drawn one after another are one draw. Returns
        /// its page and rectangle, or nothing while it is not baked yet (asking queues the bake) or
        /// when it is larger than an atlas keeps. A `mask` picture is baked white where it covers,
        /// so it takes a tint when drawn (VGContext::DrawImagePremultiplied with the tint); any
        /// other picture is baked as authored.
        [[nodiscard]] virtual Optional<PictureRegion> AtlasRegion(const foundation::vg::svg::SVGDocument& document, u32 width,
                                                                  u32 height, bool mask) = 0;

        /// Returns `document` drawn at `width` by `height` device pixels: the image whose texture the
        /// renderer samples (premultiplied colour: VGContext::DrawImagePremultiplied), or null while
        /// it is not baked yet; asking queues the bake. The document must outlive the bake (a
        /// resource provider's does).
        [[nodiscard]] virtual const foundation::image::ImageData* Baked(const foundation::vg::svg::SVGDocument& document,
                                                                        u32 width, u32 height) = 0;
        /// Counts the bakes that have finished. A GUI that drew a picture as paths while it waited
        /// redraws when this changes.
        [[nodiscard]] virtual u64 Generation() const = 0;
    };
}
