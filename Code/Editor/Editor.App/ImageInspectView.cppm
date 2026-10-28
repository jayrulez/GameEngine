// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :image_inspect_view partition
//
// ImageInspectView: the picture the image pages show (Texture, Image, Heightfield, Fonts;
// Documentation/Specs/editor-gui.md, 5.4, layer 2), where they had a fitted image and nothing more
// (the catalog's pages.md, 23: no zoom, pan, channels, checkerboard or pixel readout). The picture
// sits on a checkerboard so alpha shows; the wheel zooms about the pointer, a drag (left or middle)
// pans, a double click fits it again. It fits the view until the user zooms or pans, and again
// whenever the picture's size changes. A channel (red, green, blue or alpha) shows on its own as
// grey, and the colour channels without alpha as opaque. The pixel under the pointer is told, its
// position and its RGBA8 value, so a page can say it.
//
// It holds its own copy of the pixels (RGBA8; another format shows its picture but no readout or
// channels), so a page reading its asset again just gives it the new one.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:image_inspect_view;

import foundation.core;
import foundation.image;
import foundation.gui;
import :asset_page; // FindById

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;
    namespace image = foundation::image;

    enum class ImageChannel : u8
    {
        All,   ///< as it is, alpha over the checkerboard
        Rgb,   ///< the colour channels, opaque
        Red,
        Green,
        Blue,
        Alpha,
    };

    /// A pixel's RGBA8 value.
    struct ImagePixel
    {
        u8 r = 0;
        u8 g = 0;
        u8 b = 0;
        u8 a = 0;
    };

    class ImageInspectView final : public gui::UIWidget
    {
        RTTI_OBJECT(ImageInspectView, gui::UIWidget)
    public:
        static constexpr f32 kMinZoom = 1.0f / 64.0f;
        static constexpr f32 kMaxZoom = 64.0f;

        ImageInspectView();

        /// Shows `pixels` (its own copy from now on); fitted again when its size differs from the
        /// last one's.
        void SetImage(image::Image pixels);
        /// Shows nothing (an asset with no picture yet).
        void ClearImage();
        [[nodiscard]] bool HasImage() const noexcept { return m_pixels->Width() > 0 && m_pixels->Height() > 0; }
        [[nodiscard]] const image::Image& Pixels() const noexcept { return *m_pixels; }

        void SetChannel(ImageChannel channel);
        [[nodiscard]] ImageChannel Channel() const noexcept { return m_channel; }

        /// The whole picture in the view, centred, as large as fits.
        void Fit();
        /// One picture pixel to one view pixel, about the view's middle.
        void ActualSize();
        /// `zoom` (clamped), keeping the picture's point under `local` where it is.
        void ZoomAbout(f32 zoom, Float2 local);
        [[nodiscard]] f32 Zoom() const noexcept { return m_zoom; }
        [[nodiscard]] bool Fitting() const noexcept { return m_fitting; }
        /// Where the picture's top left corner is, in the view's space.
        [[nodiscard]] Float2 Origin() const noexcept { return m_origin; }

        /// The picture's pixel at `local` (the view's space); false off the picture.
        bool PixelAt(Float2 local, i32& x, i32& y) const;
        /// Its RGBA8 value; false when there is none (off the picture, not RGBA8).
        bool ValueAt(i32 x, i32 y, ImagePixel& out) const;

        /// Told the pixel under the pointer as it moves (x < 0: none), and when the zoom changed.
        Function<void(i32 x, i32 y)> OnHover;
        Function<void()> OnZoomChanged;

        // === For tests: the gestures at points in its own space ===
        void WheelAt(Float2 local, f32 steps);
        void DragBy(Float2 from, Float2 to);

    protected:
        [[nodiscard]] Float2 MeasureContent(Float2) override { return Float2{64.0f, 64.0f}; }
        void OnLayout() override;
        void OnDraw(gui::DrawContext& ctx, const gui::Rect& localBounds) override;
        void OnMouseDown(const gui::MouseEvent& event) override;
        void OnMouseMove(const gui::MouseEvent& event) override;
        void OnMouseUp(const gui::MouseEvent& event) override;
        void OnMouseLeave(const gui::MouseEvent& event) override;
        void OnMouseWheel(const gui::WheelEvent& event) override;
        void OnPointerCancel(const gui::MouseEvent&) override { m_panning = false; }
        [[nodiscard]] gui::Cursor GetCursorNow() const override { return m_panning ? gui::Cursor::Grabbing : gui::UIWidget::GetCursorNow(); }

    private:
        /// The picture as the channel shows it, made again when the channel or the pixels change.
        [[nodiscard]] const image::ImageData* Shown();
        void Hover(Float2 local);
        void ZoomChanged();

        // Each picture a new object, never assigned over the last: an image keeps its identity
        // through an assignment, and the renderer's texture cache keys on it, so an assigned
        // picture (a font rebaked, a channel picked) drew the old texture. Never null.
        UniquePtr<image::Image> m_pixels;
        UniquePtr<image::Image> m_channelPixels; // m_pixels as a channel shows it (not All)
        bool m_channelStale = true;
        ImageChannel m_channel = ImageChannel::All;
        f32 m_zoom = 1.0f;
        Float2 m_origin{0.0f, 0.0f};
        Float2 m_size{0.0f, 0.0f}; // the view's, at the last layout
        bool m_fitting = true;
        bool m_panning = false;
        Float2 m_panFrom{0.0f, 0.0f};
        Float2 m_panOrigin{0.0f, 0.0f};
        i32 m_hoverX = -1;
        i32 m_hoverY = -1;
    };

    RTTI_DEFINE_OBJECT(ImageInspectView, "rtti::editor::app")

    /// The picture of an image page's markup, bound: the view in `image-stage`, Fit (`image-fit`),
    /// 1:1 (`image-actual`), the zoom (`image-zoom`), the channel (`image-channel`, in
    /// ImageChannel's order), the pixel under the pointer (`image-readout`), and what shows when
    /// there is no picture (`image-empty`, its words `image-empty-text`).
    class ImagePreview
    {
    public:
        explicit ImagePreview(gui::Node& root);
        ImagePreview(const ImagePreview&) = delete;
        ImagePreview& operator=(const ImagePreview&) = delete;

        /// Shows `pixels`.
        void Show(image::Image pixels);
        /// Shows no picture, `why` in its place (empty: the markup's words).
        void ShowNone(StringView why = {});
        [[nodiscard]] ImageInspectView& View() noexcept { return *m_view; }
        /// What the readout says of pixel (x, y) (x < 0: none), in place of its RGBA8 value: a
        /// page whose picture stands for something else (a heightfield's heights) says that.
        void SetDescribe(Function<String(i32 x, i32 y)> describe) { m_describe = foundation::core::Move(describe); }
        [[nodiscard]] StringView ZoomText() const;
        [[nodiscard]] StringView Readout() const;

    private:
        void ShowZoom();
        void ShowPixel(i32 x, i32 y);

        RefPtr<ImageInspectView> m_view;
        gui::Node* m_stage = nullptr;
        gui::Node* m_empty = nullptr;
        gui::Label* m_emptyText = nullptr;
        gui::Label* m_zoom = nullptr;
        gui::Label* m_readout = nullptr;
        gui::ComboBox* m_channel = nullptr;
        Function<String(i32, i32)> m_describe;
    };

    /// A picture's pixel value as a readout says it: "x 12  y 40  ·  R 255 G 128 B 0 A 255".
    [[nodiscard]] String DescribePixel(i32 x, i32 y, const ImagePixel* value);
}
