// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :image_inspect_view partition, its body. See ImageInspectView.cppm.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.app;

import foundation.core;
import foundation.image;
import foundation.gui;
import :image_inspect_view;
import :asset_page;

using namespace foundation::core;

namespace editor::app
{
    namespace
    {
        constexpr f32 kCell = 10.0f;      // a checkerboard square, in view pixels
        constexpr f32 kWheelStep = 1.25f; // one wheel notch's zoom
    }

    ImageInspectView::ImageInspectView()
    {
        m_pixels = MakeUnique<image::Image>(MemoryAllocator());
        m_channelPixels = MakeUnique<image::Image>(MemoryAllocator());
        AddClass(u8"image-inspect");
    }

    void ImageInspectView::SetImage(image::Image pixels)
    {
        const bool resized = pixels.Width() != m_pixels->Width() || pixels.Height() != m_pixels->Height();
        m_pixels = MakeUnique<image::Image>(MemoryAllocator(), foundation::core::Move(pixels)); // a new identity (a widget's own Move would move it)
        m_channelStale = true;
        if (resized)
            m_fitting = true;
        if (m_fitting)
            Fit();
        Invalidate();
    }

    void ImageInspectView::ClearImage()
    {
        m_pixels = MakeUnique<image::Image>(MemoryAllocator());
        m_channelPixels = MakeUnique<image::Image>(MemoryAllocator());
        m_channelStale = true;
        m_fitting = true;
        Invalidate();
    }

    void ImageInspectView::SetChannel(ImageChannel channel)
    {
        if (channel == m_channel)
            return;
        m_channel = channel;
        m_channelStale = true;
        Invalidate();
    }

    void ImageInspectView::Fit()
    {
        m_fitting = true;
        if (!HasImage() || m_size.x <= 0.0f || m_size.y <= 0.0f)
            return;
        const f32 w = static_cast<f32>(m_pixels->Width());
        const f32 h = static_cast<f32>(m_pixels->Height());
        m_zoom = Clamp(Min(m_size.x / w, m_size.y / h), kMinZoom, kMaxZoom);
        m_origin = Float2{0.5f * (m_size.x - w * m_zoom), 0.5f * (m_size.y - h * m_zoom)};
        ZoomChanged();
    }

    void ImageInspectView::ActualSize()
    {
        ZoomAbout(1.0f, Float2{0.5f * m_size.x, 0.5f * m_size.y});
    }

    void ImageInspectView::ZoomAbout(f32 zoom, Float2 local)
    {
        zoom = Clamp(zoom, kMinZoom, kMaxZoom);
        // The picture's point under `local` stays under it.
        const Float2 at{(local.x - m_origin.x) / m_zoom, (local.y - m_origin.y) / m_zoom};
        m_zoom = zoom;
        m_origin = Float2{local.x - at.x * m_zoom, local.y - at.y * m_zoom};
        m_fitting = false;
        ZoomChanged();
    }

    void ImageInspectView::ZoomChanged()
    {
        Invalidate();
        if (OnZoomChanged)
            OnZoomChanged();
    }

    bool ImageInspectView::PixelAt(Float2 local, i32& x, i32& y) const
    {
        if (!HasImage() || m_zoom <= 0.0f)
            return false;
        const f32 px = (local.x - m_origin.x) / m_zoom;
        const f32 py = (local.y - m_origin.y) / m_zoom;
        if (px < 0.0f || py < 0.0f || px >= static_cast<f32>(m_pixels->Width()) || py >= static_cast<f32>(m_pixels->Height()))
            return false;
        x = static_cast<i32>(px);
        y = static_cast<i32>(py);
        return true;
    }

    bool ImageInspectView::ValueAt(i32 x, i32 y, ImagePixel& out) const
    {
        if (!HasImage() || m_pixels->Format() != image::PixelFormat::RGBA8 || x < 0 || y < 0 ||
            static_cast<u32>(x) >= m_pixels->Width() || static_cast<u32>(y) >= m_pixels->Height())
            return false;
        const Span<const u8> data = m_pixels->PixelData();
        const usize at = (static_cast<usize>(y) * m_pixels->Width() + static_cast<usize>(x)) * 4u;
        if (at + 3 >= data.Size())
            return false;
        out = ImagePixel{data[at], data[at + 1], data[at + 2], data[at + 3]};
        return true;
    }

    const image::ImageData* ImageInspectView::Shown()
    {
        if (!HasImage())
            return nullptr;
        if (m_channel == ImageChannel::All || m_pixels->Format() != image::PixelFormat::RGBA8)
            return m_pixels.Get();
        if (m_channelStale)
        {
            m_channelPixels = MakeUnique<image::Image>(MemoryAllocator(), m_pixels->Width(), m_pixels->Height(), image::PixelFormat::RGBA8, m_pixels->PixelData());
            const Span<u8> data = m_channelPixels->PixelDataMut();
            for (usize i = 0; i + 3 < data.Size(); i += 4)
            {
                if (m_channel == ImageChannel::Rgb)
                {
                    data[i + 3] = 255;
                    continue;
                }
                const usize from = m_channel == ImageChannel::Red ? 0u : m_channel == ImageChannel::Green ? 1u : m_channel == ImageChannel::Blue ? 2u : 3u;
                const u8 value = data[i + from];
                data[i] = value;
                data[i + 1] = value;
                data[i + 2] = value;
                data[i + 3] = 255;
            }
            m_channelStale = false;
        }
        return m_channelPixels.Get();
    }

    void ImageInspectView::OnLayout()
    {
        gui::UIWidget::OnLayout();
        const Float2 size{GetWidth(), GetHeight()};
        if (size.x != m_size.x || size.y != m_size.y)
        {
            // Resized: a fitted picture fits again; else it keeps its middle where it was.
            const Float2 shift{0.5f * (size.x - m_size.x), 0.5f * (size.y - m_size.y)};
            m_size = size;
            if (m_fitting)
                Fit();
            else
                m_origin = Float2{m_origin.x + shift.x, m_origin.y + shift.y};
        }
    }

    void ImageInspectView::OnDraw(gui::DrawContext& ctx, const gui::Rect& localBounds)
    {
        const image::ImageData* shown = Shown();
        if (shown == nullptr)
            return;
        ctx.PushClip(localBounds);
        const f32 w = static_cast<f32>(m_pixels->Width()) * m_zoom;
        const f32 h = static_cast<f32>(m_pixels->Height()) * m_zoom;
        const gui::Rect picture{localBounds.x + m_origin.x, localBounds.y + m_origin.y, w, h};
        // The checkerboard under it, where alpha can show through: the view's own background
        // with every other square in the text's colour, faint, so it follows the theme.
        if (m_channel == ImageChannel::All)
        {
            const f32 left = Max(picture.x, localBounds.x);
            const f32 top = Max(picture.y, localBounds.y);
            const f32 right = Min(picture.x + w, localBounds.x + localBounds.width);
            const f32 bottom = Min(picture.y + h, localBounds.y + localBounds.height);
            Color ink = GetTextColor();
            ink.a *= 0.08f;
            const i32 firstColumn = static_cast<i32>(Floor((left - picture.x) / kCell));
            const i32 firstRow = static_cast<i32>(Floor((top - picture.y) / kCell));
            for (i32 row = firstRow; picture.y + static_cast<f32>(row) * kCell < bottom; ++row)
                for (i32 column = firstColumn; picture.x + static_cast<f32>(column) * kCell < right; ++column)
                {
                    if (((row + column) & 1) == 0)
                        continue;
                    const f32 x0 = Max(picture.x + static_cast<f32>(column) * kCell, left);
                    const f32 y0 = Max(picture.y + static_cast<f32>(row) * kCell, top);
                    const f32 x1 = Min(picture.x + static_cast<f32>(column + 1) * kCell, right);
                    const f32 y1 = Min(picture.y + static_cast<f32>(row + 1) * kCell, bottom);
                    ctx.VG().FillRect(gui::Rect{x0, y0, x1 - x0, y1 - y0}.ToRectangle(), ink);
                }
        }
        ctx.VG().DrawImage(shown, picture.ToRectangle(),
                           Rectangle{0.0f, 0.0f, static_cast<f32>(shown->Width()), static_cast<f32>(shown->Height())}, Color::White);
        ctx.PopClip();
    }

    void ImageInspectView::OnMouseDown(const gui::MouseEvent& event)
    {
        if (event.Button != gui::MouseButton::Left && event.Button != gui::MouseButton::Middle)
            return;
        event.Handled = true;
        if (event.Button == gui::MouseButton::Left && event.ClickCount == 2)
        {
            Fit();
            return;
        }
        m_panning = true;
        m_panFrom = ConvertToNodeSpace(event.Position);
        m_panOrigin = m_origin;
    }

    void ImageInspectView::OnMouseMove(const gui::MouseEvent& event)
    {
        const Float2 local = ConvertToNodeSpace(event.Position);
        if (m_panning)
        {
            m_origin = Float2{m_panOrigin.x + local.x - m_panFrom.x, m_panOrigin.y + local.y - m_panFrom.y};
            m_fitting = false;
            Invalidate();
        }
        Hover(local);
    }

    void ImageInspectView::OnMouseUp(const gui::MouseEvent& event)
    {
        if (m_panning && (event.Button == gui::MouseButton::Left || event.Button == gui::MouseButton::Middle))
        {
            m_panning = false;
            event.Handled = true;
        }
    }

    void ImageInspectView::OnMouseLeave(const gui::MouseEvent&)
    {
        if (m_hoverX >= 0 && !m_panning)
        {
            m_hoverX = -1;
            m_hoverY = -1;
            if (OnHover)
                OnHover(-1, -1);
        }
    }

    void ImageInspectView::OnMouseWheel(const gui::WheelEvent& event)
    {
        const f32 steps = event.Delta.y != 0.0f ? event.Delta.y : event.Delta.x;
        if (steps == 0.0f || !HasImage())
            return;
        event.Handled = true;
        WheelAt(ConvertToNodeSpace(event.Position), steps);
    }

    void ImageInspectView::WheelAt(Float2 local, f32 steps)
    {
        ZoomAbout(m_zoom * Pow(kWheelStep, steps), local);
        Hover(local);
    }

    void ImageInspectView::DragBy(Float2 from, Float2 to)
    {
        m_origin = Float2{m_origin.x + to.x - from.x, m_origin.y + to.y - from.y};
        m_fitting = false;
        Invalidate();
    }

    void ImageInspectView::Hover(Float2 local)
    {
        i32 x = -1;
        i32 y = -1;
        if (!PixelAt(local, x, y))
        {
            x = -1;
            y = -1;
        }
        if (x == m_hoverX && y == m_hoverY)
            return;
        m_hoverX = x;
        m_hoverY = y;
        if (OnHover)
            OnHover(x, y);
    }

    // === ImagePreview ===

    ImagePreview::ImagePreview(gui::Node& root)
    {
        m_view = MakeRef<ImageInspectView>(EditorRootAllocator());
        m_stage = FindById<gui::Node>(root, u8"image-stage");
        if (m_stage != nullptr)
            m_stage->AddChild(m_view.Get());
        m_empty = FindById<gui::Node>(root, u8"image-empty");
        m_emptyText = FindById<gui::Label>(root, u8"image-empty-text");
        m_zoom = FindById<gui::Label>(root, u8"image-zoom");
        m_readout = FindById<gui::Label>(root, u8"image-readout");
        m_channel = FindById<gui::ComboBox>(root, u8"image-channel");
        ImagePreview* self = this;
        if (auto* fit = FindById<gui::ButtonBase>(root, u8"image-fit"))
            fit->SetOnClick([self]() { self->m_view->Fit(); });
        if (auto* actual = FindById<gui::ButtonBase>(root, u8"image-actual"))
            actual->SetOnClick([self]() { self->m_view->ActualSize(); });
        if (m_channel != nullptr)
            m_channel->SetOnSelectionChanged([self](i32 index)
                                             {
                                                 if (index >= 0 && index <= static_cast<i32>(ImageChannel::Alpha))
                                                     self->m_view->SetChannel(static_cast<ImageChannel>(index));
                                             });
        m_view->OnZoomChanged = [self]() { self->ShowZoom(); };
        m_view->OnHover = [self](i32 x, i32 y) { self->ShowPixel(x, y); };
        ShowNone();
    }

    void ImagePreview::Show(image::Image pixels)
    {
        m_view->SetImage(Move(pixels));
        if (m_stage != nullptr)
            m_stage->SetDisplayed(true);
        if (m_empty != nullptr)
            m_empty->SetDisplayed(false);
        ShowZoom();
    }

    void ImagePreview::ShowNone(StringView why)
    {
        m_view->ClearImage();
        if (m_stage != nullptr)
            m_stage->SetDisplayed(false);
        if (m_empty != nullptr)
            m_empty->SetDisplayed(true);
        if (m_emptyText != nullptr && !why.IsEmpty())
            m_emptyText->SetText(why);
        ShowPixel(-1, -1);
    }

    StringView ImagePreview::ZoomText() const
    {
        return m_zoom != nullptr ? m_zoom->GetText() : StringView{};
    }

    StringView ImagePreview::Readout() const
    {
        return m_readout != nullptr ? m_readout->GetText() : StringView{};
    }

    void ImagePreview::ShowZoom()
    {
        if (m_zoom != nullptr)
            m_zoom->SetText(Format(u8"{}%", static_cast<i32>(m_view->Zoom() * 100.0f + 0.5f)).AsView());
    }

    void ImagePreview::ShowPixel(i32 x, i32 y)
    {
        if (m_readout == nullptr)
            return;
        if (m_describe)
        {
            m_readout->SetText(x < 0 ? StringView{} : m_describe(x, y).AsView());
            return;
        }
        ImagePixel value;
        const bool known = m_view->ValueAt(x, y, value);
        m_readout->SetText(DescribePixel(x, y, known ? &value : nullptr).AsView());
    }

    String DescribePixel(i32 x, i32 y, const ImagePixel* value)
    {
        if (x < 0 || y < 0)
            return String{};
        if (value == nullptr)
            return Format(u8"x {}  y {}", x, y);
        return Format(u8"x {}  y {}  ·  R {} G {} B {} A {}", x, y, static_cast<u32>(value->r), static_cast<u32>(value->g), static_cast<u32>(value->b), static_cast<u32>(value->a));
    }
}
