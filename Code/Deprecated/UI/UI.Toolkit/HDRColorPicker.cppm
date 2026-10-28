// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :hdr_color_picker partition
//
// Interactive HDR-allowed color picker. Mirrors ColorPicker's SV square + hue strip + alpha strip, but the
// widgets drive a normalized [0,1] LDR color while a separate intensity multiplier scales RGB into HDR.
// Output is a Float4 (Vector4) with HDR-allowed channels and an alpha clamped to [0,1]. Ported from
// Sedulous.UI.Toolkit/src/HDRColorPicker.bf (a ViewGroup).
//
// Same inner-class idiom as ColorPicker: public nested SVSquare / HueStripView / AlphaStripView with their
// OnDraw / UpdateFromMouse defined out-of-line once HDRColorPicker is complete. Beef Vector4 .X/.Y/.Z/.W ->
// Float4 .x/.y/.z/.w; byte `Color(r,g,b,a)` -> private static Rgb(); float `Color(r,g,b,a)` -> Color{...}.

module;
#include <cmath>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:hdr_color_picker;

import foundation.core;
import foundation.vg;
import foundation.ui;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui::toolkit
{
    /// Interactive HDR color picker driving a normalized color + separate intensity multiplier.
    class HDRColorPicker : public ViewGroup
    {
        RTTI_OBJECT(HDRColorPicker, ViewGroup)
    public:
        Event<void(HDRColorPicker*, Float4)> OnColorChanged;

        HDRColorPicker();

        /// Get the current color (HDR-allowed Float4 RGBA).
        [[nodiscard]] Float4 CurrentColor() const
        {
            return HSVIToVec4(m_hue, m_saturation, m_value, m_intensity, m_alpha);
        }

        /// Set the current color (HDR Float4) and update all sub-views.
        void SetColor(Float4 color)
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;
            Vec4ToHSVI(color, m_hue, m_saturation, m_value, m_intensity, m_alpha);
            SyncViewsFromState();
            m_syncing = false;
        }

        void SetOriginalColor(Float4 color)
        {
            m_originalColor = color;
            m_previewOriginal->Color.SetValue(ClampToLDRColor(color));
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            if (Drawable* bg = ResolveStyleDrawable(StyleProperty::Background))
            {
                bg->Draw(ctx, Rectangle{0, 0, Width(), Height()});
            }
            else
            {
                ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()}, Rgb(42, 44, 54, 255));
            }
            DrawChildren(ctx);
        }

        // === HSV + intensity decomposition helpers (ported verbatim) ===

        /// Combine HSV + intensity + alpha into an HDR Float4 (RGB = HSVToRGB(h,s,v) * intensity).
        [[nodiscard]] static Float4 HSVIToVec4(f32 h, f32 s, f32 v, f32 i, f32 a)
        {
            f32 r, g, b;
            HSVToRGB(h, s, v, r, g, b);
            return Float4{r * i, g * i, b * i, a};
        }

        /// Decompose an HDR Float4 into HSV + intensity + alpha. Intensity is max(R,G,B); normalized
        /// color is RGB/intensity. On true black, HSV is left untouched so the SV indicator does not snap.
        static void Vec4ToHSVI(Float4 color, f32& h, f32& s, f32& v, f32& i, f32& a)
        {
            a = core::Clamp(color.w, 0.0f, 1.0f);
            const f32 maxChan = core::Max(color.x, core::Max(color.y, color.z));
            if (maxChan <= 0.0001f)
            {
                i = 0;
                return;
            }
            i = maxChan;
            const f32 inv = 1.0f / maxChan;
            const f32 r = core::Clamp(color.x * inv, 0.0f, 1.0f);
            const f32 g = core::Clamp(color.y * inv, 0.0f, 1.0f);
            const f32 b = core::Clamp(color.z * inv, 0.0f, 1.0f);
            RGBToHSV(r, g, b, h, s, v);
        }

        /// Standard HSV -> linear RGB in [0,1]^3.
        static void HSVToRGB(f32 h, f32 s, f32 v, f32& r, f32& g, f32& b)
        {
            const f32 c = v * s;
            const f32 hPrime = h / 60.0f;
            const f32 x = c * (1.0f - Abs(std::fmod(hPrime, 2.0f) - 1.0f));
            const f32 m = v - c;

            f32 r1 = 0, g1 = 0, b1 = 0;
            if (hPrime < 1)
            {
                r1 = c;
                g1 = x;
            }
            else if (hPrime < 2)
            {
                r1 = x;
                g1 = c;
            }
            else if (hPrime < 3)
            {
                g1 = c;
                b1 = x;
            }
            else if (hPrime < 4)
            {
                g1 = x;
                b1 = c;
            }
            else if (hPrime < 5)
            {
                r1 = x;
                b1 = c;
            }
            else
            {
                r1 = c;
                b1 = x;
            }

            r = r1 + m;
            g = g1 + m;
            b = b1 + m;
        }

        /// Linear RGB in [0,1]^3 -> HSV.
        static void RGBToHSV(f32 r, f32 g, f32 b, f32& h, f32& s, f32& v)
        {
            const f32 cMax = core::Max(r, core::Max(g, b));
            const f32 cMin = core::Min(r, core::Min(g, b));
            const f32 delta = cMax - cMin;

            v = cMax;
            s = (cMax == 0) ? 0 : delta / cMax;

            if (delta == 0)
            {
                h = 0;
            }
            else if (cMax == r)
            {
                h = 60.0f * std::fmod((g - b) / delta, 6.0f);
            }
            else if (cMax == g)
            {
                h = 60.0f * (((b - r) / delta) + 2.0f);
            }
            else
            {
                h = 60.0f * (((r - g) / delta) + 4.0f);
            }

            if (h < 0)
            {
                h += 360.0f;
            }
        }

        // === Inner views (mirror ColorPicker's, but display the normalized [0,1] color) ===

        class SVSquare : public View
        {
            RTTI_OBJECT(SVSquare, View)
        public:
            explicit SVSquare(HDRColorPicker* picker) : m_picker(picker) {}

            void OnDraw(UIDrawContext& ctx) override;
            void OnMouseDown(MouseEventArgs& e) override
            {
                if (e.Button != MouseButton::Left)
                {
                    return;
                }
                m_dragging = true;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                UpdateFromMouse(e.X, e.Y);
                e.Handled = true;
            }
            void OnMouseMove(MouseEventArgs& e) override
            {
                if (m_dragging)
                {
                    UpdateFromMouse(e.X, e.Y);
                }
            }
            void OnMouseUp(MouseEventArgs& e) override
            {
                if (m_dragging && e.Button == MouseButton::Left)
                {
                    m_dragging = false;
                    if (Context != nullptr)
                    {
                        Context->GetFocusManager()->ReleaseCapture();
                    }
                    e.Handled = true;
                }
            }

        private:
            void UpdateFromMouse(f32 x, f32 y);
            HDRColorPicker* m_picker;
            bool m_dragging = false;
        };

        class HueStripView : public View
        {
            RTTI_OBJECT(HueStripView, View)
        public:
            explicit HueStripView(HDRColorPicker* picker) : m_picker(picker) {}

            void OnDraw(UIDrawContext& ctx) override;
            void OnMouseDown(MouseEventArgs& e) override
            {
                if (e.Button != MouseButton::Left)
                {
                    return;
                }
                m_dragging = true;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                UpdateFromMouse(e.Y);
                e.Handled = true;
            }
            void OnMouseMove(MouseEventArgs& e) override
            {
                if (m_dragging)
                {
                    UpdateFromMouse(e.Y);
                }
            }
            void OnMouseUp(MouseEventArgs& e) override
            {
                if (m_dragging && e.Button == MouseButton::Left)
                {
                    m_dragging = false;
                    if (Context != nullptr)
                    {
                        Context->GetFocusManager()->ReleaseCapture();
                    }
                    e.Handled = true;
                }
            }

        private:
            void UpdateFromMouse(f32 y);
            HDRColorPicker* m_picker;
            bool m_dragging = false;
        };

        class AlphaStripView : public View
        {
            RTTI_OBJECT(AlphaStripView, View)
        public:
            explicit AlphaStripView(HDRColorPicker* picker) : m_picker(picker) {}

            void OnDraw(UIDrawContext& ctx) override;
            void OnMouseDown(MouseEventArgs& e) override
            {
                if (e.Button != MouseButton::Left)
                {
                    return;
                }
                m_dragging = true;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                UpdateFromMouse(e.Y);
                e.Handled = true;
            }
            void OnMouseMove(MouseEventArgs& e) override
            {
                if (m_dragging)
                {
                    UpdateFromMouse(e.Y);
                }
            }
            void OnMouseUp(MouseEventArgs& e) override
            {
                if (m_dragging && e.Button == MouseButton::Left)
                {
                    m_dragging = false;
                    if (Context != nullptr)
                    {
                        Context->GetFocusManager()->ReleaseCapture();
                    }
                    e.Handled = true;
                }
            }

        private:
            void UpdateFromMouse(f32 y);
            HDRColorPicker* m_picker;
            bool m_dragging = false;
        };

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 inputsW = 100.0f;
            const f32 totalW =
                m_squareSize + m_gap + m_stripWidth + m_gap + m_stripWidth + m_gap + inputsW;
            MeasuredSize = Float2{constraints.ConstrainWidth(totalW),
                                  constraints.ConstrainHeight(m_squareSize)};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            const f32 h = height;
            const f32 w = width;
            const f32 sqSize = core::Min(m_squareSize, h);
            f32 x = 0;

            m_svSquare->Measure(BoxConstraints::Tight(sqSize, sqSize));
            m_svSquare->Layout(x, 0, sqSize, sqSize);
            x += sqSize + m_gap;

            m_hueStrip->Measure(BoxConstraints::Tight(m_stripWidth, sqSize));
            m_hueStrip->Layout(x, 0, m_stripWidth, sqSize);
            x += m_stripWidth + m_gap;

            m_alphaStrip->Measure(BoxConstraints::Tight(m_stripWidth, sqSize));
            m_alphaStrip->Layout(x, 0, m_stripWidth, sqSize);
            x += m_stripWidth + m_gap;

            const f32 inputW = core::Max(w - x, 80.0f);
            const f32 inputH = 22.0f;
            f32 y = 0;

            const f32 previewH = 28.0f;
            const f32 halfW = (inputW - 4.0f) * 0.5f;
            m_previewCurrent->Measure(BoxConstraints::Tight(halfW, previewH));
            m_previewCurrent->Layout(x, y, halfW, previewH);
            m_previewOriginal->Measure(BoxConstraints::Tight(halfW, previewH));
            m_previewOriginal->Layout(x + halfW + 4.0f, y, halfW, previewH);
            y += previewH + 8.0f;

            m_intensityField->Measure(BoxConstraints::Tight(inputW, inputH));
            m_intensityField->Layout(x, y, inputW, inputH);
            y += inputH + 6.0f;

            m_rField->Measure(BoxConstraints::Tight(inputW, inputH));
            m_rField->Layout(x, y, inputW, inputH);
            y += inputH + 4.0f;

            m_gField->Measure(BoxConstraints::Tight(inputW, inputH));
            m_gField->Layout(x, y, inputW, inputH);
            y += inputH + 4.0f;

            m_bField->Measure(BoxConstraints::Tight(inputW, inputH));
            m_bField->Layout(x, y, inputW, inputH);
            y += inputH + 4.0f;

            m_aField->Measure(BoxConstraints::Tight(inputW, inputH));
            m_aField->Layout(x, y, inputW, inputH);
        }

    private:
        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        [[nodiscard]] static Color ClampToLDRColor(Float4 c)
        {
            return Color{core::Clamp(c.x, 0.0f, 1.0f), core::Clamp(c.y, 0.0f, 1.0f),
                         core::Clamp(c.z, 0.0f, 1.0f), core::Clamp(c.w, 0.0f, 1.0f)};
        }

        void SyncFromHSV()
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;
            SyncViewsFromState();
            OnColorChanged.Invoke(this, CurrentColor());
            m_syncing = false;
        }

        void SyncViewsFromState()
        {
            const Float4 color = HSVIToVec4(m_hue, m_saturation, m_value, m_intensity, m_alpha);
            m_rField->SetValue(color.x);
            m_gField->SetValue(color.y);
            m_bField->SetValue(color.z);
            m_aField->SetValue(color.w);
            m_intensityField->SetValue(m_intensity);
            m_previewCurrent->Color.SetValue(ClampToLDRColor(color));
        }

        void SyncFromRGB()
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;
            const f32 r = static_cast<f32>(m_rField->Value());
            const f32 g = static_cast<f32>(m_gField->Value());
            const f32 b = static_cast<f32>(m_bField->Value());
            Vec4ToHSVI(Float4{r, g, b, m_alpha}, m_hue, m_saturation, m_value, m_intensity,
                       m_alpha);
            SyncViewsFromState();
            OnColorChanged.Invoke(this, CurrentColor());
            m_syncing = false;
        }

        void SyncFromIntensity()
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;
            m_intensity = static_cast<f32>(m_intensityField->Value());
            SyncViewsFromState();
            OnColorChanged.Invoke(this, CurrentColor());
            m_syncing = false;
        }

        void SyncFromAlpha()
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;
            m_alpha = static_cast<f32>(m_aField->Value());
            SyncViewsFromState();
            OnColorChanged.Invoke(this, CurrentColor());
            m_syncing = false;
        }

        // Normalized HSV/A drive the picker widgets; intensity multiplies RGB.
        f32 m_hue = 0.0f;        // 0-360
        f32 m_saturation = 1.0f; // 0-1
        f32 m_value = 1.0f;      // 0-1
        f32 m_alpha = 1.0f;      // 0-1
        f32 m_intensity = 1.0f;  // 0-8 (HDR range; not hard-capped)
        Float4 m_originalColor = Float4{1, 1, 1, 1};
        bool m_syncing = false;

        // Inner views - borrowed raw (the child tree owns the RefPtr).
        SVSquare* m_svSquare = nullptr;
        HueStripView* m_hueStrip = nullptr;
        AlphaStripView* m_alphaStrip = nullptr;
        NumericField* m_intensityField = nullptr;
        NumericField* m_rField = nullptr;
        NumericField* m_gField = nullptr;
        NumericField* m_bField = nullptr;
        NumericField* m_aField = nullptr;
        ColorView* m_previewCurrent = nullptr;
        ColorView* m_previewOriginal = nullptr;

        // Layout constants.
        f32 m_squareSize = 180.0f;
        f32 m_stripWidth = 20.0f;
        f32 m_gap = 8.0f;
    };

    // === HDRColorPicker constructor (needs the complete inner-view types) ===

    inline HDRColorPicker::HDRColorPicker()
    {
        RefPtr<SVSquare> sv = MakeRef<SVSquare>(MemoryAllocator(), this);
        m_svSquare = sv.Get();
        AddView(sv.Get());

        RefPtr<HueStripView> hue = MakeRef<HueStripView>(MemoryAllocator(), this);
        m_hueStrip = hue.Get();
        AddView(hue.Get());

        RefPtr<AlphaStripView> alpha = MakeRef<AlphaStripView>(MemoryAllocator(), this);
        m_alphaStrip = alpha.Get();
        AddView(alpha.Get());

        RefPtr<ColorView> prevCur = MakeRef<ColorView>(MemoryAllocator());
        prevCur->Color.SetValue(Color::White);
        m_previewCurrent = prevCur.Get();
        AddView(prevCur.Get());

        RefPtr<ColorView> prevOrig = MakeRef<ColorView>(MemoryAllocator());
        prevOrig->Color.SetValue(Color::White);
        m_previewOriginal = prevOrig.Get();
        AddView(prevOrig.Get());

        RefPtr<NumericField> intensity = MakeRef<NumericField>(MemoryAllocator());
        intensity->SetMin(0);
        intensity->SetMax(64);
        intensity->SetStep(0.1);
        intensity->SetDecimalPlaces(3);
        intensity->SetValue(1);
        intensity->SetPrefix(StringView(u8"Int"));
        m_intensityField = intensity.Get();
        {
            HDRColorPicker* self = this;
            intensity->OnValueChanged.Add([self](NumericField*, f64)
                                          { self->SyncFromIntensity(); });
        }
        AddView(intensity.Get());

        auto makeHDRField = [this](StringView prefix) -> RefPtr<NumericField>
        {
            RefPtr<NumericField> f = MakeRef<NumericField>(MemoryAllocator());
            f->SetMin(0);
            f->SetMax(64);
            f->SetStep(0.01);
            f->SetDecimalPlaces(3);
            f->SetValue(1);
            f->SetPrefix(prefix);
            HDRColorPicker* self = this;
            f->OnValueChanged.Add([self](NumericField*, f64) { self->SyncFromRGB(); });
            return f;
        };

        RefPtr<NumericField> rf = makeHDRField(StringView(u8"R"));
        m_rField = rf.Get();
        AddView(rf.Get());

        RefPtr<NumericField> gf = makeHDRField(StringView(u8"G"));
        m_gField = gf.Get();
        AddView(gf.Get());

        RefPtr<NumericField> bf = makeHDRField(StringView(u8"B"));
        m_bField = bf.Get();
        AddView(bf.Get());

        RefPtr<NumericField> af = MakeRef<NumericField>(MemoryAllocator());
        af->SetMin(0);
        af->SetMax(1);
        af->SetStep(0.01);
        af->SetDecimalPlaces(3);
        af->SetValue(1);
        af->SetPrefix(StringView(u8"A"));
        m_aField = af.Get();
        {
            HDRColorPicker* self = this;
            af->OnValueChanged.Add([self](NumericField*, f64) { self->SyncFromAlpha(); });
        }
        AddView(af.Get());

        m_originalColor = CurrentColor();
        SyncViewsFromState();
    }

    // === Inner-view out-of-line bodies (need the complete HDRColorPicker type) ===

    inline void HDRColorPicker::SVSquare::OnDraw(UIDrawContext& ctx)
    {
        const i32 steps = 30;
        const f32 cellW = Width() / steps;
        const f32 cellH = Height() / steps;

        for (i32 iy = 0; iy < steps; ++iy)
        {
            const f32 v = 1.0f - static_cast<f32>(iy) / (steps - 1);
            for (i32 ix = 0; ix < steps; ++ix)
            {
                const f32 s = static_cast<f32>(ix) / (steps - 1);
                f32 r, g, b;
                HSVToRGB(m_picker->m_hue, s, v, r, g, b);
                ctx.VG().FillRect(Rectangle{ix * cellW, iy * cellH, cellW + 1, cellH + 1},
                                  Color{r, g, b, 1.0f});
            }
        }

        const f32 cx = m_picker->m_saturation * Width();
        const f32 cy = (1.0f - m_picker->m_value) * Height();
        const Color indicatorColor =
            (m_picker->m_value > 0.5f) ? Rgb(0, 0, 0, 255) : Rgb(255, 255, 255, 255);
        ctx.VG().StrokeCircle(Float2{cx, cy}, 5, indicatorColor, 2);

        const Color border = ResolveStyleColor(StyleProperty::BorderColor, Rgb(80, 85, 100, 255));
        ctx.VG().StrokeRect(Rectangle{0, 0, Width(), Height()}, border, 1);
    }

    inline void HDRColorPicker::SVSquare::UpdateFromMouse(f32 x, f32 y)
    {
        m_picker->m_saturation = core::Clamp(x / Width(), 0.0f, 1.0f);
        m_picker->m_value = core::Clamp(1.0f - y / Height(), 0.0f, 1.0f);
        m_picker->SyncFromHSV();
    }

    inline void HDRColorPicker::HueStripView::OnDraw(UIDrawContext& ctx)
    {
        const i32 steps = 36;
        const f32 cellH = Height() / steps;

        for (i32 i = 0; i < steps; ++i)
        {
            const f32 hue = static_cast<f32>(i) / (steps - 1) * 360.0f;
            f32 r, g, b;
            HSVToRGB(hue, 1, 1, r, g, b);
            ctx.VG().FillRect(Rectangle{0, i * cellH, Width(), cellH + 1}, Color{r, g, b, 1.0f});
        }

        const f32 iy = (m_picker->m_hue / 360.0f) * Height();
        ctx.VG().FillRect(Rectangle{0, iy - 1, Width(), 3}, Rgb(255, 255, 255, 230));
        ctx.VG().StrokeRect(Rectangle{0, iy - 1, Width(), 3}, Rgb(0, 0, 0, 128), 1);

        const Color border = ResolveStyleColor(StyleProperty::BorderColor, Rgb(80, 85, 100, 255));
        ctx.VG().StrokeRect(Rectangle{0, 0, Width(), Height()}, border, 1);
    }

    inline void HDRColorPicker::HueStripView::UpdateFromMouse(f32 y)
    {
        m_picker->m_hue = core::Clamp(y / Height(), 0.0f, 1.0f) * 360.0f;
        m_picker->SyncFromHSV();
    }

    inline void HDRColorPicker::AlphaStripView::OnDraw(UIDrawContext& ctx)
    {
        const f32 checkSize = 5;
        const Color light = Rgb(200, 200, 200, 255);
        const Color dark = Rgb(128, 128, 128, 255);

        const i32 cols = static_cast<i32>(Ceil(Width() / checkSize));
        const i32 rows = static_cast<i32>(Ceil(Height() / checkSize));
        for (i32 ry = 0; ry < rows; ++ry)
        {
            for (i32 cx = 0; cx < cols; ++cx)
            {
                const Color c = ((ry + cx) % 2 == 0) ? light : dark;
                ctx.VG().FillRect(Rectangle{cx * checkSize, ry * checkSize,
                                            core::Min(checkSize, Width() - cx * checkSize),
                                            core::Min(checkSize, Height() - ry * checkSize)},
                                  c);
            }
        }

        f32 r, g, b;
        HSVToRGB(m_picker->m_hue, m_picker->m_saturation, m_picker->m_value, r, g, b);
        const i32 steps = 20;
        const f32 cellH = Height() / steps;
        for (i32 i = 0; i < steps; ++i)
        {
            const f32 alpha = 1.0f - static_cast<f32>(i) / (steps - 1);
            ctx.VG().FillRect(Rectangle{0, i * cellH, Width(), cellH + 1}, Color{r, g, b, alpha});
        }

        const f32 iy = (1.0f - m_picker->m_alpha) * Height();
        ctx.VG().FillRect(Rectangle{0, iy - 1, Width(), 3}, Rgb(255, 255, 255, 230));
        ctx.VG().StrokeRect(Rectangle{0, iy - 1, Width(), 3}, Rgb(0, 0, 0, 128), 1);

        const Color border = ResolveStyleColor(StyleProperty::BorderColor, Rgb(80, 85, 100, 255));
        ctx.VG().StrokeRect(Rectangle{0, 0, Width(), Height()}, border, 1);
    }

    inline void HDRColorPicker::AlphaStripView::UpdateFromMouse(f32 y)
    {
        m_picker->m_alpha = core::Clamp(1.0f - y / Height(), 0.0f, 1.0f);
        m_picker->SyncFromHSV();
    }

    RTTI_DEFINE_OBJECT(HDRColorPicker, "rtti::ui::toolkit")
    RTTI_DEFINE_OBJECT(HDRColorPicker::SVSquare, "rtti::ui::toolkit")
    RTTI_DEFINE_OBJECT(HDRColorPicker::HueStripView, "rtti::ui::toolkit")
    RTTI_DEFINE_OBJECT(HDRColorPicker::AlphaStripView, "rtti::ui::toolkit")
}
