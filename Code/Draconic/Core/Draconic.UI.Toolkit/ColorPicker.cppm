// Draconic UI Toolkit - :color_picker partition
//
// Interactive HSV color picker: SV square + hue strip + alpha strip + RGB number fields + hex input +
// current/original preview swatches. Ported from Sedulous.UI.Toolkit/src/ColorPicker.bf (a ViewGroup).
//
// The Beef private inner classes (SVSquare / HueStripView / AlphaStripView) become PUBLIC nested View
// subclasses so each can carry its own DRACONIC_OBJECT identity (defined out-of-line at namespace scope).
// Their OnDraw / UpdateFromMouse dereference the enclosing ColorPicker (incomplete inside the class body),
// so those bodies are defined out-of-line after ColorPicker is complete - the same idiom Toolbar uses.
// Beef `new`/owned children -> borrowed raw T* (the ViewGroup child tree owns the RefPtr). Beef byte
// `Color(r,g,b,a)` literals -> a private static Rgb() helper; float `Color(r,g,b,a)` -> Color{...}.
// `Math.Abs(hPrime % 2)` -> Abs(std::fmod(...)); hex format/parse ported inline (core has no hex helpers).

module;
#include <cmath>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:color_picker;

import draconic.foundation;
import draconic.vg;
import draconic.ui;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Interactive HSV color picker with SV square, hue strip, alpha strip, RGB fields, hex input,
    /// and current/original preview swatches.
    class ColorPicker : public ViewGroup
    {
        DRACONIC_OBJECT(ColorPicker, ViewGroup)
    public:
        Event<void(ColorPicker*, Color)> OnColorChanged;

        ColorPicker();

        /// Get the current color.
        [[nodiscard]] Color CurrentColor() const
        {
            return HSVToRGB(m_hue, m_saturation, m_value, m_alpha);
        }

        /// Set the current color and update all sub-views.
        void SetColor(Color color)
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;
            m_alpha = color.a;
            RGBToHSV(color.r, color.g, color.b, m_hue, m_saturation, m_value);
            SyncViewsFromHSV();
            m_syncing = false;
        }

        /// Set the original color (shown in the "original" preview swatch).
        void SetOriginalColor(Color color)
        {
            m_originalColor = color;
            m_previewOriginal->Color.SetValue(color);
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

        // === HSV helpers (ported verbatim) ===

        [[nodiscard]] static Color HSVToRGB(f32 h, f32 s, f32 v, f32 a = 1.0f)
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

            return Color{r1 + m, g1 + m, b1 + m, a};
        }

        static void RGBToHSV(f32 r, f32 g, f32 b, f32& h, f32& s, f32& v)
        {
            const f32 cMax = foundation::Max(r, foundation::Max(g, b));
            const f32 cMin = foundation::Min(r, foundation::Min(g, b));
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

        // === Inner views (public nested so each carries a DRACONIC_OBJECT identity) ===

        /// Saturation/Value square: S on X-axis, V on Y-axis (inverted).
        class SVSquare : public View
        {
            DRACONIC_OBJECT(SVSquare, View)
        public:
            explicit SVSquare(ColorPicker* picker) : m_picker(picker) {}

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
            ColorPicker* m_picker;
            bool m_dragging = false;
        };

        /// Vertical hue rainbow strip.
        class HueStripView : public View
        {
            DRACONIC_OBJECT(HueStripView, View)
        public:
            explicit HueStripView(ColorPicker* picker) : m_picker(picker) {}

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
            ColorPicker* m_picker;
            bool m_dragging = false;
        };

        /// Vertical alpha strip with checkerboard background.
        class AlphaStripView : public View
        {
            DRACONIC_OBJECT(AlphaStripView, View)
        public:
            explicit AlphaStripView(ColorPicker* picker) : m_picker(picker) {}

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
            ColorPicker* m_picker;
            bool m_dragging = false;
        };

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 inputsW = 80.0f;
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
            const f32 sqSize = foundation::Min(m_squareSize, h);
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

            const f32 inputW = foundation::Max(w - x, 70.0f);
            const f32 inputH = 24.0f;
            f32 y = 0;

            const f32 previewH = 28.0f;
            const f32 halfW = (inputW - 4.0f) * 0.5f;
            m_previewCurrent->Measure(BoxConstraints::Tight(halfW, previewH));
            m_previewCurrent->Layout(x, y, halfW, previewH);
            m_previewOriginal->Measure(BoxConstraints::Tight(halfW, previewH));
            m_previewOriginal->Layout(x + halfW + 4.0f, y, halfW, previewH);
            y += previewH + 8.0f;

            m_hexInput->Measure(BoxConstraints::Tight(inputW, inputH));
            m_hexInput->Layout(x, y, inputW, inputH);
            y += inputH + 6.0f;

            m_rField->Measure(BoxConstraints::Tight(inputW, inputH));
            m_rField->Layout(x, y, inputW, inputH);
            y += inputH + 4.0f;

            m_gField->Measure(BoxConstraints::Tight(inputW, inputH));
            m_gField->Layout(x, y, inputW, inputH);
            y += inputH + 4.0f;

            m_bField->Measure(BoxConstraints::Tight(inputW, inputH));
            m_bField->Layout(x, y, inputW, inputH);
        }

    private:
        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        // === Internal sync ===

        void SyncFromHSV()
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;
            SyncViewsFromHSV();
            OnColorChanged.Invoke(this, CurrentColor());
            m_syncing = false;
        }

        void SyncViewsFromHSV()
        {
            const Color color = HSVToRGB(m_hue, m_saturation, m_value, m_alpha);

            m_rField->SetValue(Round(color.r * 255.0f));
            m_gField->SetValue(Round(color.g * 255.0f));
            m_bField->SetValue(Round(color.b * 255.0f));

            String hex;
            hex.Append(u8"#");
            hex.Append(ToHex2(static_cast<i32>(Round(color.r * 255.0f))));
            hex.Append(ToHex2(static_cast<i32>(Round(color.g * 255.0f))));
            hex.Append(ToHex2(static_cast<i32>(Round(color.b * 255.0f))));
            m_hexInput->SetText(hex);

            m_previewCurrent->Color.SetValue(color);
        }

        void SyncFromRGB()
        {
            if (m_syncing)
            {
                return;
            }
            m_syncing = true;

            const f32 r = static_cast<f32>(m_rField->Value()) / 255.0f;
            const f32 g = static_cast<f32>(m_gField->Value()) / 255.0f;
            const f32 b = static_cast<f32>(m_bField->Value()) / 255.0f;

            RGBToHSV(r, g, b, m_hue, m_saturation, m_value);
            SyncViewsFromHSV();
            OnColorChanged.Invoke(this, CurrentColor());
            m_syncing = false;
        }

        void OnHexSubmit()
        {
            if (m_syncing)
            {
                return;
            }

            StringView view = m_hexInput->Text();
            usize start = 0;
            if (view.Size() > 0 && view[0] == static_cast<char8_t>('#'))
            {
                start = 1;
            }
            const usize len = view.Size() - start;
            if (len != 6)
            {
                return;
            }

            u32 hexVal = 0;
            for (usize i = 0; i < 6; ++i)
            {
                const i32 digit = HexDigit(view[start + i]);
                if (digit < 0)
                {
                    return;
                }
                hexVal = (hexVal << 4) | static_cast<u32>(digit);
            }

            const f32 r = ((hexVal >> 16) & 0xFFu) / 255.0f;
            const f32 g = ((hexVal >> 8) & 0xFFu) / 255.0f;
            const f32 b = (hexVal & 0xFFu) / 255.0f;

            m_syncing = true;
            RGBToHSV(r, g, b, m_hue, m_saturation, m_value);
            SyncViewsFromHSV();
            OnColorChanged.Invoke(this, CurrentColor());
            m_syncing = false;
        }

        [[nodiscard]] static String ToHex2(i32 v)
        {
            static const char8_t* digits = u8"0123456789ABCDEF";
            const char8_t buf[2] = {digits[(v >> 4) & 0xF], digits[v & 0xF]};
            return String(StringView(buf, 2));
        }

        [[nodiscard]] static i32 HexDigit(char8_t c)
        {
            if (c >= static_cast<char8_t>('0') && c <= static_cast<char8_t>('9'))
            {
                return c - static_cast<char8_t>('0');
            }
            if (c >= static_cast<char8_t>('a') && c <= static_cast<char8_t>('f'))
            {
                return 10 + (c - static_cast<char8_t>('a'));
            }
            if (c >= static_cast<char8_t>('A') && c <= static_cast<char8_t>('F'))
            {
                return 10 + (c - static_cast<char8_t>('A'));
            }
            return -1;
        }

        f32 m_hue = 0.0f;        // 0-360
        f32 m_saturation = 1.0f; // 0-1
        f32 m_value = 1.0f;      // 0-1
        f32 m_alpha = 1.0f;      // 0-1
        Color m_originalColor = Color::White;
        bool m_syncing = false;

        // Inner views - borrowed raw (the child tree owns the RefPtr).
        SVSquare* m_svSquare = nullptr;
        HueStripView* m_hueStrip = nullptr;
        AlphaStripView* m_alphaStrip = nullptr;
        EditText* m_hexInput = nullptr;
        NumericField* m_rField = nullptr;
        NumericField* m_gField = nullptr;
        NumericField* m_bField = nullptr;
        ColorView* m_previewCurrent = nullptr;
        ColorView* m_previewOriginal = nullptr;

        // Layout constants.
        f32 m_squareSize = 180.0f;
        f32 m_stripWidth = 20.0f;
        f32 m_gap = 8.0f;
    };

    // === ColorPicker constructor (needs the complete inner-view types) ===

    inline ColorPicker::ColorPicker()
    {
        RefPtr<SVSquare> sv = MakeRef<SVSquare>(DefaultAllocator(), this);
        m_svSquare = sv.Get();
        AddView(sv.Get());

        RefPtr<HueStripView> hue = MakeRef<HueStripView>(DefaultAllocator(), this);
        m_hueStrip = hue.Get();
        AddView(hue.Get());

        RefPtr<AlphaStripView> alpha = MakeRef<AlphaStripView>(DefaultAllocator(), this);
        m_alphaStrip = alpha.Get();
        AddView(alpha.Get());

        RefPtr<ColorView> prevCur = MakeRef<ColorView>(DefaultAllocator());
        prevCur->Color.SetValue(Color::White);
        m_previewCurrent = prevCur.Get();
        AddView(prevCur.Get());

        RefPtr<ColorView> prevOrig = MakeRef<ColorView>(DefaultAllocator());
        prevOrig->Color.SetValue(Color::White);
        m_previewOriginal = prevOrig.Get();
        AddView(prevOrig.Get());

        RefPtr<EditText> hex = MakeRef<EditText>(DefaultAllocator());
        hex->SetPlaceholder(u8"#RRGGBB");
        hex->MaxLength.SetValue(7);
        m_hexInput = hex.Get();
        {
            ColorPicker* self = this;
            hex->OnSubmit.Add([self](EditText*) { self->OnHexSubmit(); });
        }
        AddView(hex.Get());

        auto makeRgbField = [this]() -> RefPtr<NumericField>
        {
            RefPtr<NumericField> f = MakeRef<NumericField>(DefaultAllocator());
            f->SetMin(0);
            f->SetMax(255);
            f->SetStep(1);
            f->SetValue(255);
            ColorPicker* self = this;
            f->OnValueChanged.Add([self](NumericField*, f64) { self->SyncFromRGB(); });
            return f;
        };

        RefPtr<NumericField> rf = makeRgbField();
        m_rField = rf.Get();
        AddView(rf.Get());

        RefPtr<NumericField> gf = makeRgbField();
        m_gField = gf.Get();
        AddView(gf.Get());

        RefPtr<NumericField> bf = makeRgbField();
        m_bField = bf.Get();
        AddView(bf.Get());

        m_originalColor = CurrentColor();
        SyncViewsFromHSV();
    }

    // === Inner-view out-of-line bodies (need the complete ColorPicker type) ===

    inline void ColorPicker::SVSquare::OnDraw(UIDrawContext& ctx)
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
                const Color color = HSVToRGB(m_picker->m_hue, s, v);
                ctx.VG().FillRect(Rectangle{ix * cellW, iy * cellH, cellW + 1, cellH + 1}, color);
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

    inline void ColorPicker::SVSquare::UpdateFromMouse(f32 x, f32 y)
    {
        m_picker->m_saturation = foundation::Clamp(x / Width(), 0.0f, 1.0f);
        m_picker->m_value = foundation::Clamp(1.0f - y / Height(), 0.0f, 1.0f);
        m_picker->SyncFromHSV();
    }

    inline void ColorPicker::HueStripView::OnDraw(UIDrawContext& ctx)
    {
        const i32 steps = 36;
        const f32 cellH = Height() / steps;

        for (i32 i = 0; i < steps; ++i)
        {
            const f32 hue = static_cast<f32>(i) / (steps - 1) * 360.0f;
            const Color color = HSVToRGB(hue, 1, 1);
            ctx.VG().FillRect(Rectangle{0, i * cellH, Width(), cellH + 1}, color);
        }

        const f32 iy = (m_picker->m_hue / 360.0f) * Height();
        ctx.VG().FillRect(Rectangle{0, iy - 1, Width(), 3}, Rgb(255, 255, 255, 230));
        ctx.VG().StrokeRect(Rectangle{0, iy - 1, Width(), 3}, Rgb(0, 0, 0, 128), 1);

        const Color border = ResolveStyleColor(StyleProperty::BorderColor, Rgb(80, 85, 100, 255));
        ctx.VG().StrokeRect(Rectangle{0, 0, Width(), Height()}, border, 1);
    }

    inline void ColorPicker::HueStripView::UpdateFromMouse(f32 y)
    {
        m_picker->m_hue = foundation::Clamp(y / Height(), 0.0f, 1.0f) * 360.0f;
        m_picker->SyncFromHSV();
    }

    inline void ColorPicker::AlphaStripView::OnDraw(UIDrawContext& ctx)
    {
        // Checkerboard background.
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
                                            foundation::Min(checkSize, Width() - cx * checkSize),
                                            foundation::Min(checkSize, Height() - ry * checkSize)},
                                  c);
            }
        }

        // Color gradient from opaque (top) to transparent (bottom).
        const Color baseColor =
            HSVToRGB(m_picker->m_hue, m_picker->m_saturation, m_picker->m_value);
        const i32 steps = 20;
        const f32 cellH = Height() / steps;
        for (i32 i = 0; i < steps; ++i)
        {
            const f32 alpha = 1.0f - static_cast<f32>(i) / (steps - 1);
            const Color c = Color{baseColor.r, baseColor.g, baseColor.b, alpha};
            ctx.VG().FillRect(Rectangle{0, i * cellH, Width(), cellH + 1}, c);
        }

        const f32 iy = (1.0f - m_picker->m_alpha) * Height();
        ctx.VG().FillRect(Rectangle{0, iy - 1, Width(), 3}, Rgb(255, 255, 255, 230));
        ctx.VG().StrokeRect(Rectangle{0, iy - 1, Width(), 3}, Rgb(0, 0, 0, 128), 1);

        const Color border = ResolveStyleColor(StyleProperty::BorderColor, Rgb(80, 85, 100, 255));
        ctx.VG().StrokeRect(Rectangle{0, 0, Width(), Height()}, border, 1);
    }

    inline void ColorPicker::AlphaStripView::UpdateFromMouse(f32 y)
    {
        m_picker->m_alpha = foundation::Clamp(1.0f - y / Height(), 0.0f, 1.0f);
        m_picker->SyncFromHSV();
    }

    DRACONIC_DEFINE_OBJECT(ColorPicker, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(ColorPicker::SVSquare, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(ColorPicker::HueStripView, "draconic::ui::toolkit")
    DRACONIC_DEFINE_OBJECT(ColorPicker::AlphaStripView, "draconic::ui::toolkit")
}
