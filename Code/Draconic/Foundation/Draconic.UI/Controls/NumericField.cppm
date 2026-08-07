// Draconic UI - :numeric_field partition
//
// Numeric input field with optional integrated up/down spin buttons. Self-contained: owns its own
// TextEditingBehavior and implements ITextEditHost (like EditText, but the text mirrors a clamped f64
// value + a digits/'-'/'.' input filter). Ported from Sedulous.UI/src/Controls/NumericField.bf.
//
// Port taxes: Beef `double` -> f64; get/set properties -> Value()/SetValue()/... methods; `double.Parse`
// -> foundation::ParseFloat, `text.Trim()` -> foundation::Trimmed, `{0:F2}`/round formatting -> foundation::FormatFixed
// (uniform: FormatFixed(v, 0) already yields the rounded integer). Min()/Max() member getters shadow
// foundation::Min/Max, so the two-arg clamps call them qualified (foundation::Min/foundation::Max/foundation::Clamp). Beef
// [Friend]mText/[Friend]mBehavior test access -> public Text()/Behavior().

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:numeric_field;

import draconic.foundation;
import draconic.vg;
import draconic.fonts;
import :view;
import :property;
import :event;
import :box_constraints;
import :style_property;
import :draw_context;
import :drawable;
import :rounded_rect_drawable;
import :control_state;
import :event_args;
import :input_enums;
import :enums;
import :iclipboard;
import :itext_edit_host;
import :text_editing_behavior;
import :input_filter;
import :palette;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::ui
{
    class NumericField : public View, public ITextEditHost
    {
        DRACONIC_OBJECT(NumericField, View)
    public:
        Property<f32> ButtonWidth{20.0f};
        Property<bool> ShowSpinButtons{true};

        Event<void(NumericField*, f64)> OnValueChanged;
        Event<void(NumericField*)> OnEditBegan;
        Event<void(NumericField*)> OnEditEnded;

        NumericField() : m_behavior(this)
        {
            IsFocusable = true;
            IsTabStop = true;
            WantsArrowKeys = true;
            Cursor = CursorType::IBeam;
            ButtonWidth.SetOwner(this);
            ShowSpinButtons.SetOwner(this);
            // Only allow digits, minus, and decimal point.
            InputFilter filter;
            filter.SetCustomFilter([](char32_t c)
                                   { return (c >= U'0' && c <= U'9') || c == U'-' || c == U'.'; });
            m_behavior.SetFilter(Move(filter));
            UpdateText();
        }

        // === Value / range (Beef get/set properties -> methods) ===
        [[nodiscard]] f64 Value() const noexcept { return m_value; }
        void SetValue(f64 value)
        {
            const f64 clamped = foundation::Clamp(value, m_min, m_max);
            if (m_value != clamped)
            {
                m_value = clamped;
                UpdateText();
                OnValueChanged.Invoke(this, clamped);
            }
        }
        [[nodiscard]] f64 Min() const noexcept { return m_min; }
        void SetMin(f64 value)
        {
            m_min = value;
            if (m_max < m_min)
            {
                m_max = m_min;
            }
            if (m_value < m_min)
            {
                SetValue(m_min);
            }
        }
        [[nodiscard]] f64 Max() const noexcept { return m_max; }
        void SetMax(f64 value)
        {
            m_max = value;
            if (m_min > m_max)
            {
                m_min = m_max;
            }
            if (m_value > m_max)
            {
                SetValue(m_max);
            }
        }
        [[nodiscard]] f64 Step() const noexcept { return m_step; }
        void SetStep(f64 value) { m_step = foundation::Max(0.0, value); }
        [[nodiscard]] i32 DecimalPlaces() const noexcept { return m_decimalPlaces; }
        void SetDecimalPlaces(i32 value)
        {
            m_decimalPlaces = foundation::Max(0, value);
            UpdateText();
        }

        void Increment() { SetValue(m_value + m_step); }
        void Decrement() { SetValue(m_value - m_step); }

        // Test/host access (Beef [Friend]).
        [[nodiscard]] TextEditingBehavior& Behavior() noexcept { return m_behavior; }

        /// Wants platform text input (IME) while focused - it edits its value as text.
        [[nodiscard]] bool WantsTextInput() const override { return IsEffectivelyEnabled(); }

        // === Prefix / suffix ===
        void SetPrefix(StringView text)
        {
            m_prefixView = nullptr;
            m_prefixText = String(text);
            m_hasPrefixText = true;
            Invalidate();
        }
        void SetPrefix(View* view)
        {
            m_hasPrefixText = false;
            m_prefixText.Clear();
            m_prefixView = RefPtr<View>(view);
            Invalidate();
        }
        void SetSuffix(StringView text)
        {
            m_suffixView = nullptr;
            m_suffixText = String(text);
            m_hasSuffixText = true;
            Invalidate();
        }
        void SetSuffix(View* view)
        {
            m_hasSuffixText = false;
            m_suffixText.Clear();
            m_suffixView = RefPtr<View>(view);
            Invalidate();
        }

        // === ITextEditHost ===
        [[nodiscard]] StringView Text() const override { return m_text; }
        [[nodiscard]] i32 GetMaxLength() const override { return 0; }
        [[nodiscard]] bool GetIsReadOnly() const override { return false; }
        [[nodiscard]] bool IsMultiline() const override { return false; }
        [[nodiscard]] i32 TextCharCount() const override
        {
            return static_cast<i32>(Utf8Length(m_text));
        }

        void ReplaceText(i32 charStart, i32 charLength, StringView replacement) override
        {
            const i32 byteStart = CharToByteOffset(m_text, charStart);
            const i32 byteEnd = CharToByteOffset(m_text, charStart + charLength);
            m_text.Remove(static_cast<usize>(byteStart), static_cast<usize>(byteEnd - byteStart));
            m_text.Insert(static_cast<usize>(byteStart), replacement);
            m_glyphsDirty = true;
        }

        void OnTextModified() override
        {
            m_glyphsDirty = true;
            m_cursorBlinkResetTime = Context ? Context->TotalTime() : 0.0f;
            Invalidate();

            // Live-parse the value from the text (without reformatting mid-edit).
            if (!m_updatingText)
            {
                if (Optional<f64> parsed = ParseFloat(m_text); parsed.HasValue())
                {
                    const f64 clamped = foundation::Clamp(parsed.Value(), m_min, m_max);
                    if (m_value != clamped)
                    {
                        m_value = clamped;
                        OnValueChanged.Invoke(this, m_value);
                    }
                }
            }
        }

        [[nodiscard]] i32 HitTestPosition(f32 localX, f32 localY) override
        {
            (void)localY;
            EnsureGlyphsValid();
            fonts::CachedFont* font = ResolveFont();
            if (font == nullptr || font->shaper == nullptr)
            {
                return 0;
            }
            const f32 hitX = localX - kTextPaddingLeft - GetPrefixWidth() + m_scrollOffsetX;
            return font->shaper->HitTest(*font->font, GlyphSpan(), hitX, 0).InsertionIndex();
        }
        [[nodiscard]] i32 HitTestGlyphPosition(f32 glyphX, f32 glyphY) override
        {
            return HitTestPosition(glyphX + kTextPaddingLeft + GetPrefixWidth() - m_scrollOffsetX,
                                   glyphY);
        }
        [[nodiscard]] f32 GetCursorXPosition(i32 charIndex) override
        {
            EnsureGlyphsValid();
            fonts::CachedFont* font = ResolveFont();
            if (font == nullptr || font->shaper == nullptr)
            {
                return 0.0f;
            }
            return font->shaper->GetCursorPosition(*font->font, GlyphSpan(), charIndex);
        }
        [[nodiscard]] f32 GetCursorYPosition(i32) override { return 0.0f; }
        [[nodiscard]] f32 LineHeight() override
        {
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            fonts::CachedFont* font = ResolveFont();
            return font != nullptr ? font->font->Metrics().lineHeight : fontSize;
        }
        [[nodiscard]] IClipboard* Clipboard() override
        {
            return Context ? Context->Clipboard() : nullptr;
        }
        [[nodiscard]] f32 CurrentTime() override { return Context ? Context->TotalTime() : 0.0f; }

        // === Input ===
        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled() || e.Button != MouseButton::Left)
            {
                return;
            }
            if (ShowSpinButtons.Value())
            {
                const f32 btnX = Width() - ButtonWidth.Value();
                if (e.X >= btnX)
                {
                    if (e.Y < Height() * 0.5f)
                    {
                        m_pressedButton = 1;
                        Increment();
                    }
                    else
                    {
                        m_pressedButton = -1;
                        Decrement();
                    }
                    m_repeatTimer = 0;
                    m_repeatDelay = 0.4f;
                    if (Context != nullptr)
                    {
                        Context->GetFocusManager()->SetCapture(this);
                    }
                    e.Handled = true;
                    return;
                }
            }
            // The click that FOCUSED the field keeps the select-all from OnFocusGained (type-to-
            // replace, the DCC-standard numeric UX); caret placement starts from the next click.
            if (m_selectAllClick && e.ClickCount <= 1)
            {
                m_selectAllClick = false;
                ResetBlink();
                e.Handled = true;
                return;
            }
            m_selectAllClick = false;
            if (e.ClickCount <= 1)
            {
                m_isDragging = true;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
            }
            m_behavior.HandleMouseDown(e.X, e.Y, e.ClickCount, e.Modifiers);
            ResetBlink();
            e.Handled = true;
        }
        void OnMouseMove(MouseEventArgs& e) override
        {
            if (ShowSpinButtons.Value())
            {
                const f32 btnX = Width() - ButtonWidth.Value();
                if (e.X >= btnX)
                {
                    m_hoveredButton = (e.Y < Height() * 0.5f) ? 1 : -1;
                    Cursor = CursorType::Arrow;
                }
                else
                {
                    m_hoveredButton = 0;
                    Cursor = CursorType::IBeam;
                }
            }
            if (m_isDragging)
            {
                m_behavior.HandleMouseMove(e.X, e.Y);
                ResetBlink();
            }
        }
        void OnMouseUp(MouseEventArgs& e) override
        {
            if (e.Button != MouseButton::Left)
            {
                return;
            }
            if (m_pressedButton != 0)
            {
                m_pressedButton = 0;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                e.Handled = true;
            }
            else if (m_isDragging)
            {
                m_isDragging = false;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                e.Handled = true;
            }
        }
        void OnMouseLeave() override { m_hoveredButton = 0; }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            switch (e.Key)
            {
            case KeyCode::Up:
                Increment();
                e.Handled = true;
                break;
            case KeyCode::Down:
                Decrement();
                e.Handled = true;
                break;
            case KeyCode::PageUp:
                SetValue(m_value + m_step * 10);
                e.Handled = true;
                break;
            case KeyCode::PageDown:
                SetValue(m_value - m_step * 10);
                e.Handled = true;
                break;
            case KeyCode::Return:
                CommitText();
                e.Handled = true;
                break;
            default:
                m_behavior.HandleKeyDown(e.Key, e.Modifiers);
                ResetBlink();
                e.Handled = true;
                break;
            }
        }
        void OnTextInput(TextInputEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            m_behavior.HandleTextInput(e.Character);
            ResetBlink();
            e.Handled = true;
        }
        void OnMouseWheel(MouseWheelEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            if (IsFocused())
            {
                if (e.DeltaY > 0)
                {
                    Increment();
                }
                else if (e.DeltaY < 0)
                {
                    Decrement();
                }
                e.Handled = true;
            }
        }
        // Select-all on focus: the whole value is primed for a replacing edit (tab or click).
        void OnFocusGained() override
        {
            m_behavior.SelectAll();
            m_selectAllClick = true;
            ResetBlink();
            OnEditBegan.Invoke(this);
        }
        void OnFocusLost() override
        {
            m_isDragging = false;
            m_selectAllClick = false;
            CommitText();
            OnEditEnded.Invoke(this);
        }

        void CommitText()
        {
            if (Optional<f64> parsed = ParseFloat(m_text); parsed.HasValue())
            {
                m_value = foundation::Clamp(parsed.Value(), m_min, m_max);
                OnValueChanged.Invoke(this, m_value);
            }
            UpdateText();
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            f32 textH = fontSize;
            if (fonts::CachedFont* font = ResolveFont())
            {
                textH = font->font->Metrics().lineHeight;
            }
            const f32 prefixW = GetPrefixWidth();
            const f32 suffixW = GetSuffixWidth();
            MeasuredSize = Float2{
                constraints.ConstrainWidth(80.0f + EffectiveButtonWidth() + prefixW + suffixW),
                constraints.ConstrainHeight(textH + 8.0f)};
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);

            Drawable* bgDrawable = ResolveStyleDrawable(StyleProperty::Background);
            if (bgDrawable != nullptr)
            {
                bgDrawable->Draw(ctx, bounds, GetControlState());
            }
            else
            {
                ctx.VG().FillRect(bounds,
                                  Color{30.0f / 255.0f, 32.0f / 255.0f, 42.0f / 255.0f, 1.0f});
            }

            if (ShowSpinButtons.Value())
            {
                DrawSpinButtons(ctx, bgDrawable);
            }

            if (IsFocused())
            {
                const Color accent = ResolveStyleColor(
                    StyleProperty::AccentColor,
                    ResolveStyleColor(StyleProperty::CursorColor,
                                      Color{80.0f / 255.0f, 160.0f / 255.0f, 1.0f, 1.0f}));
                if (RoundedRectDrawable* rrd = Cast<RoundedRectDrawable>(bgDrawable))
                {
                    if (!rrd->Radii.IsZero())
                    {
                        ctx.VG().StrokeRoundedRect(bounds, rrd->Radii, accent, 2.0f);
                    }
                    else
                    {
                        ctx.VG().StrokeRect(bounds, accent, 2.0f);
                    }
                }
                else
                {
                    const f32 cr = ResolveStyleFloat(StyleProperty::CornerRadius);
                    if (cr > 0)
                    {
                        ctx.VG().StrokeRoundedRect(bounds, cr, accent, 2.0f);
                    }
                    else
                    {
                        ctx.VG().StrokeRect(bounds, accent, 2.0f);
                    }
                }
            }

            const f32 prefixW = GetPrefixWidth();
            const f32 suffixW = GetSuffixWidth();
            const f32 textAreaX = kTextPaddingLeft + prefixW;
            const f32 textAreaW = TextAreaWidth();

            ctx.VG().PushClipRect(Rectangle{
                kTextPaddingLeft, 0,
                Width() - kTextPaddingLeft - kTextPaddingRight - EffectiveButtonWidth(), Height()});
            if (prefixW > 0)
            {
                DrawDecoration(ctx, true, kTextPaddingLeft, 0, Height());
            }
            if (suffixW > 0)
            {
                DrawDecoration(ctx, false, kTextPaddingLeft + prefixW + textAreaW, 0, Height());
            }
            DrawTextContent(ctx, textAreaX, textAreaW, fontSize);
            ctx.VG().PopClip();

            // Repeat timer for a held spin button (frame-approximated, faithful to Sedulous).
            if (m_pressedButton != 0)
            {
                m_repeatTimer += 1.0f / 60.0f;
                if (m_repeatTimer >= m_repeatDelay)
                {
                    if (m_pressedButton == 1)
                    {
                        Increment();
                    }
                    else
                    {
                        Decrement();
                    }
                    m_repeatDelay = m_repeatInterval;
                }
            }
        }

        // Convert a character index to a byte offset in a UTF-8 string.
        [[nodiscard]] static i32 CharToByteOffset(StringView text, i32 charIndex)
        {
            i32 charCount = 0;
            usize byteOffset = 0;
            usize i = 0;
            while (i < text.Size())
            {
                if (charCount >= charIndex)
                {
                    break;
                }
                (void)DecodeUtf8(text, i);
                charCount++;
                byteOffset = i;
            }
            if (charCount < charIndex)
            {
                return static_cast<i32>(text.Size());
            }
            return static_cast<i32>(byteOffset);
        }

    private:
        [[nodiscard]] fonts::CachedFont* ResolveFont()
        {
            if (Context == nullptr || Context->FontService() == nullptr)
            {
                return nullptr;
            }
            return Context->FontService()->GetFont(
                ResolveStyleFontFamily(), ResolveStyleFloat(StyleProperty::FontSize, 14.0f));
        }
        [[nodiscard]] Span<const fonts::GlyphPosition> GlyphSpan() const
        {
            return Span<const fonts::GlyphPosition>{m_glyphPositions.Data(),
                                                    m_glyphPositions.Size()};
        }

        [[nodiscard]] f32 EffectiveButtonWidth() const
        {
            return ShowSpinButtons.Value() ? ButtonWidth.Value() : 0.0f;
        }
        [[nodiscard]] f32 TextAreaWidth()
        {
            return Width() - EffectiveButtonWidth() - kTextPaddingLeft - kTextPaddingRight -
                   GetPrefixWidth() - GetSuffixWidth();
        }

        void UpdateText()
        {
            m_updatingText = true;
            m_text = FormatFixed(m_value, m_decimalPlaces);
            m_glyphsDirty = true;
            m_behavior.Reset();
            const i32 charCount = TextCharCount();
            m_behavior.SetCursorPosition(charCount);
            m_behavior.SetAnchorPosition(charCount);
            m_updatingText = false;
        }

        void ResetBlink()
        {
            m_cursorBlinkResetTime = Context ? Context->TotalTime() : 0.0f;
            Invalidate();
        }

        void EnsureGlyphsValid()
        {
            if (!m_glyphsDirty)
            {
                return;
            }
            m_glyphsDirty = false;
            m_glyphPositions.Clear();
            m_textWidth = 0;

            fonts::CachedFont* font = ResolveFont();
            if (font == nullptr || m_text.IsEmpty())
            {
                return;
            }
            if (font->shaper != nullptr)
            {
                Result<f32> r = font->shaper->ShapeText(*font->font, m_text, m_glyphPositions);
                if (r.HasValue())
                {
                    m_textWidth = r.Value();
                }
            }
            else
            {
                m_textWidth = font->font->MeasureString(m_text, m_glyphPositions);
            }
        }

        void EnsureCursorVisible(fonts::CachedFont* font)
        {
            if (font == nullptr || font->shaper == nullptr)
            {
                return;
            }
            const f32 cursorX = font->shaper->GetCursorPosition(*font->font, GlyphSpan(),
                                                                m_behavior.CursorPosition());
            const f32 contentW = TextAreaWidth();
            if (cursorX - m_scrollOffsetX < 0)
            {
                m_scrollOffsetX = cursorX;
            }
            else if (cursorX - m_scrollOffsetX > contentW)
            {
                m_scrollOffsetX = cursorX - contentW;
            }
            m_scrollOffsetX =
                foundation::Clamp(m_scrollOffsetX, 0.0f, foundation::Max(0.0f, m_textWidth - contentW));
        }

        void DrawTextContent(UIDrawContext& ctx, f32 areaX, f32 areaW, f32 fontSize)
        {
            (void)areaW;
            (void)fontSize;
            if (ctx.FontService() == nullptr)
            {
                return;
            }
            fonts::CachedFont* font = ResolveFont();
            if (font == nullptr)
            {
                return;
            }

            const f32 lineH = font->font->Metrics().lineHeight;
            const f32 textY = (Height() - lineH) * 0.5f;
            EnsureGlyphsValid();
            EnsureCursorVisible(font);
            const f32 textX = areaX - m_scrollOffsetX;

            if (IsFocused() && m_behavior.IsSelecting() && font->shaper != nullptr)
            {
                const Color selColor = ResolveStyleColor(
                    StyleProperty::SelectionColor,
                    Color{60.0f / 255.0f, 120.0f / 255.0f, 200.0f / 255.0f, 80.0f / 255.0f});
                const f32 selStart = font->shaper->GetCursorPosition(*font->font, GlyphSpan(),
                                                                     m_behavior.SelectionStart());
                const f32 selEnd = font->shaper->GetCursorPosition(*font->font, GlyphSpan(),
                                                                   m_behavior.SelectionEnd());
                ctx.VG().FillRect(Rectangle{textX + selStart, textY, selEnd - selStart, lineH},
                                  selColor);
            }

            if (m_glyphPositions.Size() > 0)
            {
                Color textColor = ResolveStyleColor(
                    StyleProperty::TextColor,
                    Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
                if (!IsEffectivelyEnabled())
                {
                    textColor = Palette::ComputeDisabled(textColor);
                }
                ctx.VG().DrawPositionedGlyphs(m_glyphPositions, font, textX,
                                              textY + font->font->Metrics().ascent, textColor);
            }

            if (IsFocused())
            {
                const f32 elapsed =
                    (Context ? Context->TotalTime() : 0.0f) - m_cursorBlinkResetTime;
                if ((static_cast<i32>(elapsed / 0.5f) % 2) == 0)
                {
                    const f32 cursorX =
                        font->shaper != nullptr
                            ? font->shaper->GetCursorPosition(*font->font, GlyphSpan(),
                                                              m_behavior.CursorPosition())
                            : 0.0f;
                    const Color cursorColor = ResolveStyleColor(
                        StyleProperty::CursorColor,
                        Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
                    ctx.VG().FillRect(Rectangle{textX + cursorX - 1, textY, 2, lineH}, cursorColor);
                }
            }
        }

        void DrawSpinButtons(UIDrawContext& ctx, Drawable* bgDrawable)
        {
            const f32 btnX = Width() - ButtonWidth.Value();
            const f32 halfH = Height() * 0.5f;
            const Color btnBorder =
                ResolveStyleColor(StyleProperty::BorderColor,
                                  Color{80.0f / 255.0f, 85.0f / 255.0f, 100.0f / 255.0f, 1.0f});

            const ControlState upState =
                (m_pressedButton == 1)
                    ? ControlState::Pressed
                    : ((m_hoveredButton == 1) ? ControlState::Hover : ControlState::Normal);
            if (Drawable* upDrawable =
                    ResolvePartDrawable(u8"spin-up", StyleProperty::Background, upState))
            {
                upDrawable->Draw(ctx, Rectangle{btnX, 0, ButtonWidth.Value(), halfH}, upState);
            }
            else
            {
                Color upBg{50.0f / 255.0f, 55.0f / 255.0f, 68.0f / 255.0f, 1.0f};
                if (m_pressedButton == 1)
                {
                    upBg = Palette::ComputePressed(upBg);
                }
                else if (m_hoveredButton == 1)
                {
                    upBg = Palette::ComputeHover(upBg);
                }
                ctx.VG().FillRect(Rectangle{btnX, 0, ButtonWidth.Value(), halfH}, upBg);
            }

            const ControlState downState =
                (m_pressedButton == -1)
                    ? ControlState::Pressed
                    : ((m_hoveredButton == -1) ? ControlState::Hover : ControlState::Normal);
            if (Drawable* downDrawable =
                    ResolvePartDrawable(u8"spin-down", StyleProperty::Background, downState))
            {
                downDrawable->Draw(ctx, Rectangle{btnX, halfH, ButtonWidth.Value(), halfH},
                                   downState);
            }
            else
            {
                Color downBg{50.0f / 255.0f, 55.0f / 255.0f, 68.0f / 255.0f, 1.0f};
                if (m_pressedButton == -1)
                {
                    downBg = Palette::ComputePressed(downBg);
                }
                else if (m_hoveredButton == -1)
                {
                    downBg = Palette::ComputeHover(downBg);
                }
                ctx.VG().FillRect(Rectangle{btnX, halfH, ButtonWidth.Value(), halfH}, downBg);
            }

            Color sepColor = btnBorder;
            if (RoundedRectDrawable* rrd = Cast<RoundedRectDrawable>(bgDrawable))
            {
                sepColor = rrd->BorderColor;
            }
            ctx.VG().FillRect(Rectangle{btnX, 1, 1, Height() - 2}, sepColor);
            ctx.VG().FillRect(Rectangle{btnX, halfH, ButtonWidth.Value(), 1}, sepColor);

            const Color arrowColor =
                ResolveStyleColor(StyleProperty::TextColor,
                                  Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
            const f32 arrowSz = foundation::Min(ButtonWidth.Value(), halfH) * 0.25f;
            const f32 cx = btnX + ButtonWidth.Value() * 0.5f;

            // Up arrow.
            {
                const f32 cy = halfH * 0.5f;
                ctx.VG().BeginPath();
                ctx.VG().MoveTo(cx - arrowSz, cy + arrowSz * 0.5f);
                ctx.VG().LineTo(cx + arrowSz, cy + arrowSz * 0.5f);
                ctx.VG().LineTo(cx, cy - arrowSz * 0.5f);
                ctx.VG().ClosePath();
                ctx.VG().Fill(arrowColor);
            }
            // Down arrow.
            {
                const f32 cy = halfH + halfH * 0.5f;
                ctx.VG().BeginPath();
                ctx.VG().MoveTo(cx - arrowSz, cy - arrowSz * 0.5f);
                ctx.VG().LineTo(cx + arrowSz, cy - arrowSz * 0.5f);
                ctx.VG().LineTo(cx, cy + arrowSz * 0.5f);
                ctx.VG().ClosePath();
                ctx.VG().Fill(arrowColor);
            }
        }

        void DrawDecoration(UIDrawContext& ctx, bool isPrefix, f32 x, f32 y, f32 height)
        {
            const bool hasText = isPrefix ? m_hasPrefixText : m_hasSuffixText;
            const String& decoText = isPrefix ? m_prefixText : m_suffixText;
            View* decoView = (isPrefix ? m_prefixView : m_suffixView).Get();

            if (hasText && !decoText.IsEmpty())
            {
                if (fonts::CachedFont* font = ResolveFont())
                {
                    const Color textColor =
                        ResolveStyleColor(StyleProperty::TextDimColor,
                                          ResolveStyleColor(StyleProperty::PlaceholderColor,
                                                            Color{140.0f / 255.0f, 150.0f / 255.0f,
                                                                  170.0f / 255.0f, 1.0f}));
                    const f32 w = font->font->MeasureString(decoText);
                    ctx.VG().DrawText(decoText, font, Rectangle{x, y, w, height},
                                      fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle,
                                      textColor);
                }
            }
            else if (decoView != nullptr)
            {
                SyncDecoContext(decoView);
                const f32 pw = decoView->MeasuredSize.x, ph = decoView->MeasuredSize.y;
                const f32 py = y + (height - ph) * 0.5f;
                decoView->Layout(x, py, pw, ph);
                ctx.VG().PushState();
                ctx.VG().Translate(x, py);
                decoView->OnDraw(ctx);
                ctx.VG().PopState();
            }
        }

        // A prefix/suffix View is not in the child tree, so it has no Context - give it the field's so
        // it can resolve fonts (else it measures to 0 and the value text overlaps it).
        void SyncDecoContext(View* v)
        {
            if (v != nullptr && v->Context != Context)
            {
                v->Context = Context;
            }
        }

        [[nodiscard]] f32 GetPrefixWidth()
        {
            if (m_hasPrefixText && !m_prefixText.IsEmpty())
            {
                if (fonts::CachedFont* font = ResolveFont())
                {
                    return font->font->MeasureString(m_prefixText) + 4.0f;
                }
            }
            else if (m_prefixView)
            {
                SyncDecoContext(m_prefixView.Get());
                m_prefixView->Measure(BoxConstraints::Loose(200, 200));
                return m_prefixView->MeasuredSize.x + 4.0f;
            }
            return 0.0f;
        }
        [[nodiscard]] f32 GetSuffixWidth()
        {
            if (m_hasSuffixText && !m_suffixText.IsEmpty())
            {
                if (fonts::CachedFont* font = ResolveFont())
                {
                    return font->font->MeasureString(m_suffixText) + 4.0f;
                }
            }
            else if (m_suffixView)
            {
                SyncDecoContext(m_suffixView.Get());
                m_suffixView->Measure(BoxConstraints::Loose(200, 200));
                return m_suffixView->MeasuredSize.x + 4.0f;
            }
            return 0.0f;
        }

        static constexpr f32 kTextPaddingLeft = 6.0f;
        static constexpr f32 kTextPaddingRight = 6.0f;

        f64 m_value = 0.0;
        f64 m_min = 0.0;
        f64 m_max = 100.0;
        f64 m_step = 1.0;
        i32 m_decimalPlaces = 0;

        String m_text;
        TextEditingBehavior m_behavior;
        bool m_updatingText = false;

        Array<fonts::GlyphPosition> m_glyphPositions;
        bool m_glyphsDirty = true;
        f32 m_textWidth = 0.0f;
        f32 m_scrollOffsetX = 0.0f;

        f32 m_cursorBlinkResetTime = 0.0f;
        bool m_isDragging = false;
        bool m_selectAllClick = false; // the focusing click keeps the select-all (see OnMouseDown)

        String m_prefixText;
        String m_suffixText;
        bool m_hasPrefixText = false;
        bool m_hasSuffixText = false;
        RefPtr<View> m_prefixView;
        RefPtr<View> m_suffixView;

        i32 m_hoveredButton = 0; // 0=none, 1=up, -1=down
        i32 m_pressedButton = 0;
        f32 m_repeatTimer = 0.0f;
        f32 m_repeatDelay = 0.4f;
        f32 m_repeatInterval = 0.05f;
    };

    DRACONIC_DEFINE_OBJECT(NumericField, "draconic::ui")
}
