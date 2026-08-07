// Draconic UI - :edit_text partition
//
// Single-line and multiline text input control. Implements ITextEditHost for TextEditingBehavior;
// supports selection, cursor, clipboard, undo/redo, input filtering, prefix/suffix decorations.
// Ported from Sedulous.UI/src/Controls/EditText.bf.
//
// Text rendering / glyph shaping are now LIVE (the Fonts service is wired into UIContext and VG has
// glyph drawing): a glyph-position cache (m_glyphPositions) is (re)shaped on demand, hit-testing /
// cursor geometry go through the shaper, and OnDraw paints selection, positioned glyphs, cursor, and
// placeholder. The no-service / no-shaper paths keep the earlier fallbacks. The right-click Cut/Copy/
// Paste/Select-All ContextMenu is now wired (ShowContextMenu, via :context_menu).
//
// The host accessors GetMaxLength()/GetIsReadOnly() are the `Get`-prefixed ITextEditHost members
// (EditText's identically-named Property<> fields would otherwise collide - see :itext_edit_host).
// Beef `[Friend]mBehavior` test access -> a public Behavior() accessor; DecodedChars -> core codepoint
// decode; font.Shaper/font.Font -> CachedFont::shaper/::font.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:edit_text;

import draconic.foundation; // String, StringView, RefPtr, Array, Span, Utf8Length, DecodeUtf8, Clamp, Max, Min
import draconic.vg;
import draconic.fonts; // CachedFont, GlyphPosition, SelectionRange, Rectangle, TextAlignment, VerticalAlignment
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
import :context_menu;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::ui
{
    class EditText : public View, public ITextEditHost
    {
        DRACONIC_OBJECT(EditText, View)
    public:
        // === Text state (faithful public Property<> fields; tests drive these) ===
        Property<String> Placeholder;
        Property<bool> IsReadOnly{false};
        Property<bool> Multiline{false};
        Property<i32> MaxLength{0};

        /// Whether right-click shows the Cut/Copy/Paste context menu (menu itself deferred).
        bool ShowContextMenuOnRightClick = true;

        // === Events ===
        Event<void(EditText*)> OnTextChanged;
        Event<void(EditText*)> OnSubmit;

        EditText() : m_behavior(this)
        {
            IsFocusable = true;
            IsTabStop = true;
            WantsArrowKeys = true;
            Cursor = CursorType::IBeam;

            Placeholder.SetOwner(this, InvalidationKind::Visual);
            IsReadOnly.SetOwner(this, InvalidationKind::Visual);
            Multiline.SetOwner(this);
            MaxLength.SetOwner(this);

            EditText* self = this;
            Multiline.Changed.Add(
                Event<void(bool)>::Handler{[self](bool) { self->m_glyphsDirty = true; }});
        }

        // === Public text API ===
        void SetText(StringView text)
        {
            m_text = String(text);
            m_glyphsDirty = true;
            m_behavior.Reset();
            Invalidate();
        }

        void SetPlaceholder(StringView text)
        {
            Placeholder.SetValue(String(text));
            Invalidate();
        }

        // === Behavior passthrough (Beef properties -> methods; [Friend] access -> Behavior()) ===
        [[nodiscard]] TextEditingBehavior& Behavior() noexcept { return m_behavior; }

        /// Wants platform text input (IME) while focused, unless read-only. Covers PasswordBox and
        /// EditableLabel (which toggles IsReadOnly between label/edit mode).
        [[nodiscard]] bool WantsTextInput() const override
        {
            return IsEffectivelyEnabled() && !IsReadOnly.Value();
        }
        [[nodiscard]] InputFilter* Filter() { return m_behavior.Filter(); }
        void SetFilter(InputFilter filter) { m_behavior.SetFilter(Move(filter)); }

        [[nodiscard]] i32 CursorPosition() const noexcept { return m_behavior.CursorPosition(); }
        [[nodiscard]] i32 SelectionStart() const noexcept { return m_behavior.SelectionStart(); }
        [[nodiscard]] i32 SelectionEnd() const noexcept { return m_behavior.SelectionEnd(); }

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
        [[nodiscard]] i32 GetMaxLength() const override { return MaxLength.Value(); }
        [[nodiscard]] bool GetIsReadOnly() const override { return IsReadOnly.Value(); }
        [[nodiscard]] bool IsMultiline() const override { return Multiline.Value(); }
        [[nodiscard]] i32 TextCharCount() const override
        {
            return static_cast<i32>(Utf8Length(m_text));
        }

        void ReplaceText(i32 charStart, i32 charLength, StringView replacement) override
        {
            const i32 byteStart = CharToByteOffset(m_text, charStart);
            const i32 byteEnd = CharToByteOffset(m_text, charStart + charLength);
            const i32 byteLength = byteEnd - byteStart;
            m_text.Remove(static_cast<usize>(byteStart), static_cast<usize>(byteLength));
            m_text.Insert(static_cast<usize>(byteStart), replacement);
            m_glyphsDirty = true;
        }

        void OnTextModified() override
        {
            m_glyphsDirty = true;
            m_cursorBlinkResetTime = Context ? Context->TotalTime() : 0.0f;
            m_needsCursorScroll = true;
            Invalidate();
            OnTextChanged.Invoke(this);
        }

        [[nodiscard]] i32 HitTestPosition(f32 localX, f32 localY) override
        {
            EnsureGlyphsValid();
            fonts::CachedFont* font = ResolveFont();
            if (font == nullptr || font->shaper == nullptr)
            {
                return FallbackHitTest(localX);
            }

            const Thickness padding =
                ResolveStyleThickness(StyleProperty::Padding, Thickness{6, 4});
            const f32 prefixW = GetPrefixWidth();
            const f32 hitX = localX - padding.Left - prefixW + m_scrollOffsetX;
            const f32 hitY = localY - padding.Top + m_scrollOffsetY;

            if (Multiline.Value())
            {
                return MultilineHitTest(font, hitX, hitY);
            }
            return font->shaper->HitTest(*font->font, GlyphSpan(), hitX, 0).InsertionIndex();
        }

        [[nodiscard]] i32 HitTestGlyphPosition(f32 glyphX, f32 glyphY) override
        {
            EnsureGlyphsValid();
            fonts::CachedFont* font = ResolveFont();
            if (font == nullptr || font->shaper == nullptr)
            {
                return 0;
            }
            if (Multiline.Value())
            {
                return MultilineHitTest(font, glyphX, glyphY);
            }
            return font->shaper->HitTest(*font->font, GlyphSpan(), glyphX, 0).InsertionIndex();
        }

        [[nodiscard]] f32 GetCursorXPosition(i32 charIndex) override
        {
            EnsureGlyphsValid();
            fonts::CachedFont* font = ResolveFont();
            if (font == nullptr || font->shaper == nullptr)
            {
                return 0.0f;
            }
            if (!Multiline.Value())
            {
                return font->shaper->GetCursorPosition(*font->font, GlyphSpan(), charIndex);
            }
            return GetMultilineCursorX(charIndex);
        }

        [[nodiscard]] f32 GetCursorYPosition(i32 charIndex) override
        {
            EnsureGlyphsValid();
            fonts::CachedFont* font = ResolveFont();
            if (font == nullptr)
            {
                return 0.0f;
            }
            return GetCursorYFromCharIndex(charIndex, font->font->Metrics().lineHeight);
        }

        [[nodiscard]] f32 LineHeight() override
        {
            fonts::CachedFont* font = ResolveFont();
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            if (font == nullptr)
            {
                return fontSize;
            }
            return font->font->Metrics().lineHeight;
        }

        /// Vertical scroll offset (px) of the multiline text - so an external gutter can align
        /// its per-line markers to the editor's visible lines.
        [[nodiscard]] f32 ScrollOffsetY() const noexcept { return m_scrollOffsetY; }

        [[nodiscard]] IClipboard* Clipboard() override
        {
            return Context ? Context->Clipboard() : nullptr;
        }
        [[nodiscard]] f32 CurrentTime() override { return Context ? Context->TotalTime() : 0.0f; }

        // === Input handlers ===
        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            if (e.Button == MouseButton::Right && ShowContextMenuOnRightClick)
            {
                ShowContextMenu(e.X, e.Y);
                e.Handled = true;
                return;
            }
            if (e.Button != MouseButton::Left)
            {
                return;
            }
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
            if (m_isDragging)
            {
                m_isDragging = false;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                e.Handled = true;
            }
        }
        void OnKeyDown(KeyEventArgs& e) override
        {
            if (!IsEffectivelyEnabled())
            {
                return;
            }
            if (e.Key == KeyCode::Return && !Multiline.Value())
            {
                OnSubmit.Invoke(this);
                e.Handled = true;
                return;
            }
            m_behavior.HandleKeyDown(e.Key, e.Modifiers);
            ResetBlink();
            e.Handled = true;
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
            if (!Multiline.Value())
            {
                return;
            }
            const Thickness padding =
                ResolveStyleThickness(StyleProperty::Padding, Thickness{6, 4});
            const f32 contentHeight = Height() - padding.TotalVertical();
            const f32 maxScrollY = Max(0.0f, m_textHeight - contentHeight);
            if (maxScrollY <= 0)
            {
                return;
            }
            const f32 lineH = LineHeight();
            m_scrollOffsetY = Clamp(m_scrollOffsetY - e.DeltaY * lineH * 3.0f, 0.0f, maxScrollY);
            Invalidate();
            e.Handled = true;
        }
        void OnFocusGained() override { ResetBlink(); }
        void OnFocusLost() override { m_isDragging = false; }
        void OnActivate() override { OnSubmit.Invoke(this); }

        // Text to display; overridden by PasswordBox for masking. Public so tests can inspect it
        // (Beef exercised it via [Friend]); it is the PasswordBox masking extension point.
        virtual void GetDisplayText(String& outText) const { outText = m_text; }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            const Thickness padding =
                ResolveStyleThickness(StyleProperty::Padding, Thickness{6, 4});
            f32 textH = fontSize;

            if (fonts::CachedFont* font = ResolveFont())
            {
                textH = font->font->Metrics().lineHeight;
                if (Multiline.Value())
                {
                    textH *= 3.0f;
                }
            }

            const f32 prefixW = GetPrefixWidth();
            const f32 suffixW = GetSuffixWidth();
            const f32 minWidth = 100.0f + prefixW + suffixW + padding.TotalHorizontal();
            const f32 totalH = textH + padding.TotalVertical();
            MeasuredSize =
                Float2{constraints.ConstrainWidth(minWidth), constraints.ConstrainHeight(totalH)};
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Rectangle bounds{0, 0, Width(), Height()};
            const f32 fontSize = ResolveStyleFloat(StyleProperty::FontSize, 14.0f);
            const Thickness padding =
                ResolveStyleThickness(StyleProperty::Padding, Thickness{6, 4});

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

            const f32 contentX = padding.Left;
            const f32 contentY = padding.Top;
            const f32 contentW = Width() - padding.TotalHorizontal();
            const f32 contentH = Height() - padding.TotalVertical();
            const f32 prefixW = GetPrefixWidth();
            const f32 suffixW = GetSuffixWidth();

            ctx.VG().PushClipRect(Rectangle{contentX, contentY, contentW, contentH});
            if (prefixW > 0)
            {
                DrawPrefix(ctx, contentX, contentY, contentH, fontSize);
            }
            if (suffixW > 0)
            {
                DrawSuffix(ctx, contentX + contentW - suffixW, contentY, contentH, fontSize);
            }
            DrawTextContent(ctx, contentX + prefixW, contentY, contentW - prefixW - suffixW,
                            contentH, fontSize);
            ctx.VG().PopClip();
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

        String m_text;
        TextEditingBehavior m_behavior;
        bool m_isDragging = false;

        // Glyph cache.
        Array<fonts::GlyphPosition> m_glyphPositions;
        String m_cachedDisplayText;
        bool m_glyphsDirty = true;
        f32 m_textWidth = 0.0f;
        f32 m_textHeight = 0.0f;

        // Scroll + blink.
        f32 m_scrollOffsetX = 0.0f;
        f32 m_scrollOffsetY = 0.0f;
        f32 m_cursorBlinkResetTime = 0.0f;
        bool m_needsCursorScroll = false;

        // Prefix / suffix decorations.
        String m_prefixText;
        String m_suffixText;
        bool m_hasPrefixText = false;
        bool m_hasSuffixText = false;
        RefPtr<View> m_prefixView;
        RefPtr<View> m_suffixView;

    private:
        // === Context menu ===

        /// Show right-click context menu with Cut/Copy/Paste/Select All.
        void ShowContextMenu(f32 localX, f32 localY)
        {
            if (Context == nullptr)
            {
                return;
            }

            RefPtr<ContextMenu> menu = MakeRef<ContextMenu>(DefaultAllocator());
            EditText* self = this;

            if (!IsReadOnly.Value())
            {
                menu->AddItem(
                    u8"Cut",
                    [self]() { self->m_behavior.HandleKeyDown(KeyCode::X, KeyModifiers::Ctrl); },
                    m_behavior.IsSelecting());
            }

            menu->AddItem(
                u8"Copy",
                [self]() { self->m_behavior.HandleKeyDown(KeyCode::C, KeyModifiers::Ctrl); },
                m_behavior.IsSelecting());

            if (!IsReadOnly.Value())
            {
                const bool hasClipText =
                    Context->Clipboard() != nullptr && Context->Clipboard()->HasText();
                menu->AddItem(
                    u8"Paste",
                    [self]() { self->m_behavior.HandleKeyDown(KeyCode::V, KeyModifiers::Ctrl); },
                    hasClipText);
            }

            menu->AddSeparator();
            menu->AddItem(u8"Select All", [self]()
                          { self->m_behavior.HandleKeyDown(KeyCode::A, KeyModifiers::Ctrl); });

            const Float2 screenPos = LocalToScreen(Float2{localX, localY});
            menu->Show(Context, screenPos.x, screenPos.y);
        }

        [[nodiscard]] fonts::CachedFont* ResolveFont()
        {
            if (Context == nullptr || Context->FontService() == nullptr)
            {
                return nullptr;
            }
            const String family = ResolveStyleFontFamily();
            return Context->FontService()->GetFont(
                family, ResolveStyleFloat(StyleProperty::FontSize, 14.0f));
        }
        [[nodiscard]] Span<const fonts::GlyphPosition> GlyphSpan() const
        {
            return Span<const fonts::GlyphPosition>{m_glyphPositions.Data(),
                                                    m_glyphPositions.Size()};
        }

        void ResetBlink()
        {
            m_cursorBlinkResetTime = Context ? Context->TotalTime() : 0.0f;
            m_needsCursorScroll = true;
            Invalidate();
        }

        // === Glyph shaping ===
        void EnsureGlyphsValid()
        {
            if (!m_glyphsDirty)
            {
                return;
            }
            m_glyphsDirty = false;
            m_glyphPositions.Clear();
            m_textWidth = 0;
            m_textHeight = 0;

            fonts::CachedFont* font = ResolveFont();
            if (font == nullptr)
            {
                return;
            }

            m_cachedDisplayText.Clear();
            GetDisplayText(m_cachedDisplayText);
            if (m_cachedDisplayText.IsEmpty())
            {
                return;
            }

            if (Multiline.Value() && font->shaper != nullptr)
            {
                const Thickness padding =
                    ResolveStyleThickness(StyleProperty::Padding, Thickness{6, 4});
                const f32 contentWidth =
                    Width() - padding.TotalHorizontal() - GetPrefixWidth() - GetSuffixWidth();
                f32 totalH = 0;
                if (font->shaper
                        ->ShapeTextWrapped(*font->font, m_cachedDisplayText, contentWidth,
                                           m_glyphPositions, totalH)
                        .IsOk())
                {
                    m_textHeight = totalH;
                    for (const fonts::GlyphPosition& gp : m_glyphPositions)
                    {
                        m_textWidth = Max(m_textWidth, gp.x + gp.advance);
                    }
                }
            }
            else if (font->shaper != nullptr)
            {
                Result<f32> r =
                    font->shaper->ShapeText(*font->font, m_cachedDisplayText, m_glyphPositions);
                if (r.HasValue())
                {
                    m_textWidth = r.Value();
                }
                m_textHeight = font->font->Metrics().lineHeight;
            }
            else
            {
                m_textWidth = font->font->MeasureString(m_cachedDisplayText, m_glyphPositions);
                m_textHeight = font->font->Metrics().lineHeight;
            }
        }

        // === Draw ===
    protected:
        // Draw text/selection/cursor into a content rect. Protected so EditableLabel can reuse it.
        void DrawTextContent(UIDrawContext& ctx, f32 areaX, f32 areaY, f32 areaW, f32 areaH,
                             f32 fontSize)
        {
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
            const f32 textY =
                Multiline.Value() ? (areaY - m_scrollOffsetY) : (areaY + (areaH - lineH) * 0.5f);

            EnsureGlyphsValid();
            if (m_needsCursorScroll)
            {
                EnsureCursorVisible(font);
                m_needsCursorScroll = false;
            }

            const f32 textX = areaX - m_scrollOffsetX;

            const StringView placeholder = Placeholder.Value();
            if (m_text.IsEmpty() && !IsFocused() && placeholder.Size() > 0)
            {
                const Color placeholderColor = ResolveStyleColor(
                    StyleProperty::PlaceholderColor,
                    Color{140.0f / 255.0f, 150.0f / 255.0f, 170.0f / 255.0f, 1.0f});
                ctx.VG().DrawText(placeholder, font, Rectangle{areaX, areaY, areaW, areaH},
                                  fonts::TextAlignment::Left,
                                  Multiline.Value() ? fonts::VerticalAlignment::Top
                                                    : fonts::VerticalAlignment::Middle,
                                  placeholderColor);
            }
            else
            {
                if (IsFocused() && m_behavior.IsSelecting() && font->shaper != nullptr)
                {
                    const Color selColor = ResolveStyleColor(
                        StyleProperty::SelectionColor,
                        Color{60.0f / 255.0f, 120.0f / 255.0f, 200.0f / 255.0f, 80.0f / 255.0f});
                    if (Multiline.Value())
                    {
                        const i32 glyphStart = CharToGlyphIndex(m_behavior.SelectionStart());
                        const i32 glyphEnd = CharToGlyphIndex(m_behavior.SelectionEnd());
                        Array<fonts::Rectangle> rects;
                        font->shaper->GetSelectionRects(*font->font, GlyphSpan(),
                                                        fonts::SelectionRange(glyphStart, glyphEnd),
                                                        lineH, rects);
                        for (const fonts::Rectangle& r : rects)
                        {
                            ctx.VG().FillRect(
                                Rectangle{textX + r.x, textY + r.y, r.width, r.height}, selColor);
                        }
                    }
                    else
                    {
                        const f32 selStart = font->shaper->GetCursorPosition(
                            *font->font, GlyphSpan(), m_behavior.SelectionStart());
                        const f32 selEnd = font->shaper->GetCursorPosition(
                            *font->font, GlyphSpan(), m_behavior.SelectionEnd());
                        ctx.VG().FillRect(
                            Rectangle{textX + selStart, textY, selEnd - selStart, lineH}, selColor);
                    }
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
            }

            if (IsFocused() && !IsReadOnly.Value())
            {
                const f32 elapsed =
                    (Context ? Context->TotalTime() : 0.0f) - m_cursorBlinkResetTime;
                const bool cursorVisible = (static_cast<i32>(elapsed / 0.5f) % 2) == 0;
                if (cursorVisible)
                {
                    f32 cursorX = 0;
                    if (font->shaper != nullptr)
                    {
                        cursorX = Multiline.Value()
                                      ? GetMultilineCursorX(m_behavior.CursorPosition())
                                      : font->shaper->GetCursorPosition(
                                            *font->font, GlyphSpan(), m_behavior.CursorPosition());
                    }
                    const f32 cursorY =
                        Multiline.Value()
                            ? GetCursorYFromCharIndex(m_behavior.CursorPosition(), lineH)
                            : 0;
                    const Color cursorColor = ResolveStyleColor(
                        StyleProperty::CursorColor,
                        Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
                    ctx.VG().FillRect(Rectangle{textX + cursorX - 1, textY + cursorY, 2, lineH},
                                      cursorColor);
                }
            }
        }

    private:
        void EnsureCursorVisible(fonts::CachedFont* font)
        {
            if (font == nullptr || font->shaper == nullptr)
            {
                return;
            }
            const f32 cursorX = Multiline.Value()
                                    ? GetMultilineCursorX(m_behavior.CursorPosition())
                                    : font->shaper->GetCursorPosition(*font->font, GlyphSpan(),
                                                                      m_behavior.CursorPosition());
            const Thickness padding =
                ResolveStyleThickness(StyleProperty::Padding, Thickness{6, 4});
            const f32 contentWidth =
                Width() - padding.TotalHorizontal() - GetPrefixWidth() - GetSuffixWidth();

            if (!Multiline.Value())
            {
                if (cursorX - m_scrollOffsetX < 0)
                {
                    m_scrollOffsetX = cursorX;
                }
                else if (cursorX - m_scrollOffsetX > contentWidth)
                {
                    m_scrollOffsetX = cursorX - contentWidth;
                }
                m_scrollOffsetX =
                    Clamp(m_scrollOffsetX, 0.0f, Max(0.0f, m_textWidth - contentWidth));
            }
            else
            {
                const f32 lineH = font->font->Metrics().lineHeight;
                const f32 contentHeight = Height() - padding.TotalVertical();
                const f32 cursorY = GetCursorYPosition(m_behavior.CursorPosition());
                if (cursorY - m_scrollOffsetY < 0)
                {
                    m_scrollOffsetY = cursorY;
                }
                else if (cursorY + lineH - m_scrollOffsetY > contentHeight)
                {
                    m_scrollOffsetY = cursorY + lineH - contentHeight;
                }
                m_scrollOffsetY =
                    Clamp(m_scrollOffsetY, 0.0f, Max(0.0f, m_textHeight - contentHeight));
            }
        }

        // === Multiline helpers ===
        [[nodiscard]] i32 MultilineHitTest(fonts::CachedFont* font, f32 hitX, f32 hitY)
        {
            const f32 lineH = font->font->Metrics().lineHeight;
            const i32 targetLine = Max(0, static_cast<i32>(hitY / lineH));
            const i32 charCount = TextCharCount();
            const i32 lineCharStart = GetCharIndexForLine(targetLine);
            if (lineCharStart >= charCount)
            {
                return charCount;
            }

            // Empty line (char at lineCharStart is '\n').
            i32 idx = 0;
            usize bi = 0;
            const StringView disp = m_cachedDisplayText;
            while (bi < disp.Size())
            {
                if (idx == lineCharStart)
                {
                    if (DecodeUtf8(disp, bi) == U'\n')
                    {
                        return lineCharStart;
                    }
                    break;
                }
                (void)DecodeUtf8(disp, bi);
                idx++;
            }
            if (hitX <= 0)
            {
                return lineCharStart;
            }

            const fonts::HitTestResult r =
                font->shaper->HitTestWrapped(*font->font, GlyphSpan(), hitX, hitY, lineH);
            return GlyphToCharIndex(r.InsertionIndex(), r.isTrailingHit);
        }

        [[nodiscard]] i32 GetCharIndexForLine(i32 line)
        {
            if (line <= 0)
            {
                return 0;
            }
            i32 currentLine = 0, idx = 0;
            usize bi = 0;
            const StringView disp = m_cachedDisplayText;
            while (bi < disp.Size())
            {
                if (DecodeUtf8(disp, bi) == U'\n')
                {
                    currentLine++;
                    if (currentLine == line)
                    {
                        return idx + 1;
                    }
                }
                idx++;
            }
            return TextCharCount();
        }

        [[nodiscard]] f32 GetMultilineCursorX(i32 charIndex)
        {
            if (m_glyphPositions.Size() == 0)
            {
                return 0.0f;
            }
            if (charIndex > 0)
            {
                i32 idx = 0;
                usize bi = 0;
                const StringView disp = m_cachedDisplayText;
                while (bi < disp.Size())
                {
                    if (idx == charIndex - 1)
                    {
                        if (DecodeUtf8(disp, bi) == U'\n')
                        {
                            return 0.0f;
                        }
                        break;
                    }
                    (void)DecodeUtf8(disp, bi);
                    idx++;
                }
            }
            if (charIndex == 0)
            {
                return 0.0f;
            }

            for (usize i = 0; i < m_glyphPositions.Size(); ++i)
            {
                if (m_glyphPositions[i].stringIndex == charIndex)
                {
                    return m_glyphPositions[i].x;
                }
                if (m_glyphPositions[i].stringIndex > charIndex)
                {
                    if (i > 0)
                    {
                        const fonts::GlyphPosition& prev = m_glyphPositions[i - 1];
                        if (m_glyphPositions[i].y != prev.y)
                        {
                            return prev.x + prev.advance;
                        }
                    }
                    return 0.0f;
                }
            }
            const fonts::GlyphPosition& last = m_glyphPositions[m_glyphPositions.Size() - 1];
            return last.x + last.advance;
        }

        [[nodiscard]] i32 GlyphToCharIndex(i32 glyphInsertionIndex, bool isTrailingHit)
        {
            if (m_glyphPositions.Size() == 0)
            {
                return 0;
            }
            if (glyphInsertionIndex <= 0)
            {
                return m_glyphPositions[0].stringIndex;
            }
            if (glyphInsertionIndex >= static_cast<i32>(m_glyphPositions.Size()))
            {
                return m_glyphPositions[m_glyphPositions.Size() - 1].stringIndex + 1;
            }
            const fonts::GlyphPosition& prevGlyph =
                m_glyphPositions[static_cast<usize>(glyphInsertionIndex - 1)];
            const fonts::GlyphPosition& nextGlyph =
                m_glyphPositions[static_cast<usize>(glyphInsertionIndex)];
            if (nextGlyph.y != prevGlyph.y && nextGlyph.stringIndex > prevGlyph.stringIndex + 1)
            {
                return isTrailingHit ? prevGlyph.stringIndex + 1 : nextGlyph.stringIndex;
            }
            return prevGlyph.stringIndex + 1;
        }

        [[nodiscard]] i32 CharToGlyphIndex(i32 charIndex)
        {
            for (usize i = 0; i < m_glyphPositions.Size(); ++i)
            {
                if (m_glyphPositions[i].stringIndex >= charIndex)
                {
                    return static_cast<i32>(i);
                }
            }
            return static_cast<i32>(m_glyphPositions.Size());
        }

        [[nodiscard]] f32 GetCursorYFromCharIndex(i32 charIndex, f32 lineHeight)
        {
            i32 line = 0, idx = 0;
            usize bi = 0;
            const StringView disp = m_cachedDisplayText;
            while (bi < disp.Size())
            {
                if (idx >= charIndex)
                {
                    break;
                }
                if (DecodeUtf8(disp, bi) == U'\n')
                {
                    line++;
                }
                idx++;
            }
            return static_cast<f32>(line) * lineHeight;
        }

        [[nodiscard]] i32 FallbackHitTest(f32 localX)
        {
            const Thickness padding =
                ResolveStyleThickness(StyleProperty::Padding, Thickness{6, 4});
            const f32 hitX = localX - padding.Left + m_scrollOffsetX;
            const i32 charCount = TextCharCount();
            if (charCount == 0 || m_textWidth <= 0)
            {
                return 0;
            }
            const f32 avgCharW = m_textWidth / static_cast<f32>(charCount);
            return Clamp(static_cast<i32>(hitX / avgCharW + 0.5f), i32{0}, charCount);
        }

        // === Prefix / suffix ===
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
        void DrawPrefix(UIDrawContext& ctx, f32 x, f32 y, f32 height, f32 fontSize)
        {
            (void)fontSize;
            if (m_hasPrefixText && !m_prefixText.IsEmpty())
            {
                if (fonts::CachedFont* font = ResolveFont())
                {
                    const Color textColor =
                        ResolveStyleColor(StyleProperty::TextDimColor,
                                          ResolveStyleColor(StyleProperty::PlaceholderColor,
                                                            Color{140.0f / 255.0f, 150.0f / 255.0f,
                                                                  170.0f / 255.0f, 1.0f}));
                    ctx.VG().DrawText(
                        m_prefixText, font, Rectangle{x, y, GetPrefixWidth() - 4.0f, height},
                        fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle, textColor);
                }
            }
            else if (m_prefixView)
            {
                SyncDecoContext(m_prefixView.Get());
                const f32 pw = m_prefixView->MeasuredSize.x, ph = m_prefixView->MeasuredSize.y;
                const f32 py = y + (height - ph) * 0.5f;
                m_prefixView->Layout(x, py, pw, ph);
                ctx.VG().PushState();
                ctx.VG().Translate(x, py);
                m_prefixView->OnDraw(ctx);
                ctx.VG().PopState();
            }
        }
        void DrawSuffix(UIDrawContext& ctx, f32 x, f32 y, f32 height, f32 fontSize)
        {
            (void)fontSize;
            if (m_hasSuffixText && !m_suffixText.IsEmpty())
            {
                if (fonts::CachedFont* font = ResolveFont())
                {
                    const Color textColor =
                        ResolveStyleColor(StyleProperty::TextDimColor,
                                          ResolveStyleColor(StyleProperty::PlaceholderColor,
                                                            Color{140.0f / 255.0f, 150.0f / 255.0f,
                                                                  170.0f / 255.0f, 1.0f}));
                    ctx.VG().DrawText(
                        m_suffixText, font, Rectangle{x + 4.0f, y, GetSuffixWidth() - 4.0f, height},
                        fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle, textColor);
                }
            }
            else if (m_suffixView)
            {
                SyncDecoContext(m_suffixView.Get());
                const f32 pw = m_suffixView->MeasuredSize.x, ph = m_suffixView->MeasuredSize.y;
                const f32 py = y + (height - ph) * 0.5f;
                m_suffixView->Layout(x + 4.0f, py, pw, ph);
                ctx.VG().PushState();
                ctx.VG().Translate(x + 4.0f, py);
                m_suffixView->OnDraw(ctx);
                ctx.VG().PopState();
            }
        }
    };

    DRACONIC_DEFINE_OBJECT(EditText, "draconic::ui")
}
