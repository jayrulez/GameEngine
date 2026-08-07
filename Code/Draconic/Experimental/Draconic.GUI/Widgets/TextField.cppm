// Draconic GUI - :text_field partition
//
// TextField: a single-line, editable text control - the widget driven by the keyboard and
// text-input path. A lean Draconic-native control modeled on eepp's UITextInput (role only):
// it maintains an editable UTF-8 buffer and a caret (a byte offset at a codepoint boundary),
// plus a selection anchor. It inserts characters from OnTextInput, handles editing/navigation
// keys (backspace/delete/left/right/home/end, with Shift extending the selection) and the
// clipboard shortcuts (Ctrl+A/C/X/V) via OnKeyDown, and supports mouse selection (drag-select
// and double-click word-select). Placeholder/hint text shows while empty; an optional
// max-length caps the value (in codepoints). The caret blinks only when focused.
//
// Clipboard access is through the dispatcher's abstract IClipboard (null-safe: cut/copy/paste
// no-op without one). Multi-line editing and horizontal scroll of an over-long value are
// follow-ups; this is a full-featured single-line editor.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:text_field;

import draconic.foundation; // String, StringView, Color, Function, Move, Min, Max, Utf8*Boundary, IsWhiteSpace
import draconic.fonts; // CachedFont
import draconic.vg;    // CornerRadii
import :rect;
import :draw_context;
import :text;
import :event;
import :clipboard;
import :event_dispatcher;
import :ui_widget;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    class TextField : public UIWidget
    {
        DRACONIC_OBJECT(TextField, UIWidget)
    public:
        TextField()
        {
            SetTag(foundation::StringView(u8"textfield"));
            m_text.SetAlignment(TextHAlign::Left, TextVAlign::Middle);
            m_hintText.SetAlignment(TextHAlign::Left, TextVAlign::Middle);
            m_hintText.SetColor(m_placeholderColor);
            SetTabFocusable(true);
        }

        // === Value ===
        void SetText(foundation::StringView text)
        {
            m_value = foundation::String(text);
            m_caret = m_value.Size(); // caret to end
            m_selAnchor = m_caret;    // no selection
            SyncText();
            NotifyChanged();
        }
        [[nodiscard]] foundation::StringView GetText() const { return m_value.AsView(); }

        // Byte offset of the caret (always on a codepoint boundary).
        [[nodiscard]] usize GetCaret() const noexcept { return m_caret; }
        void SetCaret(usize byteOffset)
        {
            m_caret = foundation::Min(byteOffset, m_value.Size());
            m_selAnchor = m_caret; // collapse the selection
            Invalidate();
        }

        void SetOnTextChanged(foundation::Function<void(foundation::StringView)> callback)
        {
            m_onChanged = foundation::Move(callback);
        }

        // === Selection ===
        [[nodiscard]] bool HasSelection() const noexcept { return m_selAnchor != m_caret; }
        [[nodiscard]] usize SelectionStart() const noexcept
        {
            return foundation::Min(m_selAnchor, m_caret);
        }
        [[nodiscard]] usize SelectionEnd() const noexcept
        {
            return foundation::Max(m_selAnchor, m_caret);
        }
        [[nodiscard]] foundation::StringView SelectedText() const
        {
            return m_value.AsView().SubStr(SelectionStart(), SelectionEnd() - SelectionStart());
        }
        void SelectAll()
        {
            m_selAnchor = 0;
            m_caret = m_value.Size();
            ResetBlink();
            Invalidate();
        }
        void ClearSelection()
        {
            m_selAnchor = m_caret;
            Invalidate();
        }

        // === Placeholder / limits ===
        // Hint text shown (dimmed) while the value is empty.
        void SetPlaceholder(foundation::StringView text)
        {
            m_placeholder = foundation::String(text);
            m_hintText.SetString(m_placeholder.AsView());
            Invalidate();
        }
        [[nodiscard]] foundation::StringView GetPlaceholder() const { return m_placeholder.AsView(); }
        void SetPlaceholderColor(Color color)
        {
            m_placeholderColor = color;
            m_hintText.SetColor(color);
            Invalidate();
        }

        // Maximum length in codepoints (0 = unlimited). Inserts past the limit are truncated.
        void SetMaxLength(usize codepoints) noexcept { m_maxLength = codepoints; }
        [[nodiscard]] usize GetMaxLength() const noexcept { return m_maxLength; }

        // An enabled TextField wants platform text input while focused (the bridge enables
        // the window's IME accordingly).
        [[nodiscard]] bool WantsTextInput() const override { return IsEnabled(); }

        // === Appearance ===
        void SetFont(fonts::CachedFont* font)
        {
            m_text.SetFont(font);
            m_hintText.SetFont(font);
            Invalidate();
        }
        [[nodiscard]] fonts::CachedFont* GetFont() const { return m_text.GetFont(); }
        void SetTextColor(Color color)
        {
            m_text.SetColor(color);
            Invalidate();
        }

        // Theming hooks (CSS color / font-family reach the text).
        // Theming: text color drives the caret too by default (so it stays visible on any
        // background); a `textfield::caret` / `::selection` part can override either explicitly.
        void SetThemeTextColor(Color color) override
        {
            SetTextColor(color);
            m_caretColor = color;
            Invalidate();
        }
        void SetThemeFont(fonts::CachedFont* font) override { SetFont(font); }
        void CollectStyleParts(foundation::Array<foundation::StringView>& out) const override
        {
            out.PushBack(foundation::StringView(u8"caret"));
            out.PushBack(foundation::StringView(u8"selection"));
        }
        void SetThemePartColor(foundation::StringView part, Color color) override
        {
            if (part == foundation::StringView(u8"caret"))
                SetCaretColor(color);
            else if (part == foundation::StringView(u8"selection"))
                SetSelectionColor(color);
        }
        void SetCaretColor(Color color)
        {
            m_caretColor = color;
            Invalidate();
        }
        void SetSelectionColor(Color color)
        {
            m_selectionColor = color;
            Invalidate();
        }

        // The caret blink is advanced by the owner (SceneNode::Update path) if wired; a static
        // caret (always shown while focused) is the default when not ticked. The same clock
        // drives double-click detection (see OnMouseDown).
        void Update(f64 deltaSeconds)
        {
            m_clock += deltaSeconds;
            if (!IsFocused())
                return;
            m_blinkAccum += deltaSeconds;
            if (m_blinkAccum >= kBlinkPeriod)
            {
                m_blinkAccum -= kBlinkPeriod;
                m_caretVisible = !m_caretVisible;
                Invalidate();
            }
        }

    protected:
        void OnTextInput(const TextInputEvent& event) override
        {
            if (!IsEnabled() || event.Text.Size() == 0)
                return;
            InsertText(event.Text);
        }

        void OnKeyDown(const KeyEvent& event) override
        {
            if (!IsEnabled())
                return;
            const bool shift = (event.Modifiers & static_cast<u32>(KeyModShift)) != 0;
            const bool ctrl = (event.Modifiers & static_cast<u32>(KeyModCtrl)) != 0;
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);

            // Editing shortcuts take precedence over plain letter handling.
            if (ctrl)
            {
                switch (key)
                {
                case KeyCode::A:
                    SelectAll();
                    return;
                case KeyCode::C:
                    CopySelection();
                    return;
                case KeyCode::X:
                    CutSelection();
                    return;
                case KeyCode::V:
                    Paste();
                    return;
                default:
                    return; // other Ctrl combos ignored
                }
            }

            switch (key)
            {
            case KeyCode::Backspace:
                if (HasSelection())
                {
                    DeleteSelection();
                    NotifyChanged();
                }
                else if (m_caret > 0)
                {
                    const usize prev = foundation::Utf8PrevBoundary(m_value.AsView(), m_caret);
                    m_value.Remove(prev, m_caret - prev);
                    m_caret = prev;
                    m_selAnchor = m_caret;
                    SyncText();
                    NotifyChanged();
                }
                break;
            case KeyCode::Delete:
                if (HasSelection())
                {
                    DeleteSelection();
                    NotifyChanged();
                }
                else if (m_caret < m_value.Size())
                {
                    const usize next = foundation::Utf8NextBoundary(m_value.AsView(), m_caret);
                    m_value.Remove(m_caret, next - m_caret);
                    SyncText();
                    NotifyChanged();
                }
                break;
            case KeyCode::Left:
                if (shift)
                    m_caret = foundation::Utf8PrevBoundary(m_value.AsView(), m_caret);
                else if (HasSelection())
                    m_caret = SelectionStart();
                else
                    m_caret = foundation::Utf8PrevBoundary(m_value.AsView(), m_caret);
                if (!shift)
                    m_selAnchor = m_caret;
                break;
            case KeyCode::Right:
                if (shift)
                    m_caret = foundation::Utf8NextBoundary(m_value.AsView(), m_caret);
                else if (HasSelection())
                    m_caret = SelectionEnd();
                else
                    m_caret = foundation::Utf8NextBoundary(m_value.AsView(), m_caret);
                if (!shift)
                    m_selAnchor = m_caret;
                break;
            case KeyCode::Home:
                m_caret = 0;
                if (!shift)
                    m_selAnchor = m_caret;
                break;
            case KeyCode::End:
                m_caret = m_value.Size();
                if (!shift)
                    m_selAnchor = m_caret;
                break;
            default:
                return; // don't reset the blink for keys we ignore
            }
            ResetBlink();
            Invalidate();
        }

        void OnMouseDown(const MouseEvent& event) override
        {
            UINode::OnMouseDown(event);
            RequestFocus();
            const bool shift = (event.Modifiers & static_cast<u32>(KeyModShift)) != 0;
            const f32 localX = ConvertToNodeSpace(event.Position).x;
            const usize pos = CaretFromX(localX);

            // Double-click (a second press close in time AND position) selects the word. The
            // position gate both matches real double-click behaviour and keeps a run of
            // single clicks at different spots from ever being read as one.
            const bool nearInTime = (m_clock - m_lastClickTime) <= kDoubleClickTime;
            const f32 dx = localX - m_lastClickX;
            const bool nearInSpace = (dx < 0.0f ? -dx : dx) <= kDoubleClickSlop;
            m_lastClickTime = m_clock;
            m_lastClickX = localX;
            if (nearInTime && nearInSpace && !shift)
            {
                SelectWordAt(pos);
                m_selecting = false;
                ResetBlink();
                return;
            }

            m_caret = pos;
            if (!shift)
                m_selAnchor = pos; // plain press collapses; Shift extends from anchor
            m_selecting = true;    // begin a drag-select (pointer is captured)
            ResetBlink();
        }

        void OnMouseMove(const MouseEvent& event) override
        {
            if (!m_selecting)
                return;
            m_caret = CaretFromX(ConvertToNodeSpace(event.Position).x);
            ResetBlink();
            Invalidate();
        }

        void OnMouseUp(const MouseEvent& event) override
        {
            UINode::OnMouseUp(event);
            m_selecting = false;
        }

        void OnFocusGained() override
        {
            ResetBlink();
            Invalidate();
        }
        void OnFocusLost() override
        {
            m_caretVisible = false;
            m_selecting = false;
            Invalidate();
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect content = GetContentBounds();

            // Placeholder while empty (and only when we won't draw a caret over it awkwardly:
            // shown whether focused or not, which matches common single-line fields).
            if (m_value.Size() == 0 && m_placeholder.Size() > 0)
            {
                m_hintText.Draw(ctx, content);
            }
            else
            {
                // Selection highlight sits behind the text.
                if (IsFocused() && HasSelection())
                {
                    const f32 x0 = content.x + PrefixWidth(SelectionStart());
                    const f32 x1 = content.x + PrefixWidth(SelectionEnd());
                    const f32 h = m_text.GetLineHeight();
                    const f32 top = content.y + content.height * 0.5f - h * 0.5f;
                    ctx.VG().FillRect(foundation::Rectangle{x0, top, x1 - x0, h}, m_selectionColor);
                }
                m_text.Draw(ctx, content);
            }

            if (IsFocused() && m_caretVisible && !HasSelection())
            {
                const f32 caretX = content.x + PrefixWidth(m_caret);
                const f32 h = m_text.GetLineHeight();
                const f32 cy = content.y + content.height * 0.5f;
                const f32 top = cy - h * 0.5f;
                ctx.VG().FillRect(foundation::Rectangle{caretX, top, kCaretWidth, h}, m_caretColor);
            }
        }

    private:
        void SyncText()
        {
            m_text.SetString(m_value.AsView());
            Invalidate();
        }
        void NotifyChanged()
        {
            if (m_onChanged)
                m_onChanged(m_value.AsView());
        }
        void ResetBlink()
        {
            m_caretVisible = true;
            m_blinkAccum = 0.0;
            Invalidate();
        }

        // The dispatcher's clipboard adapter (may be null: no clipboard configured).
        [[nodiscard]] IClipboard* Clipboard()
        {
            EventDispatcher* dispatcher = GetEventDispatcher();
            return dispatcher != nullptr ? dispatcher->GetClipboard() : nullptr;
        }

        void CopySelection()
        {
            if (!HasSelection())
                return;
            if (IClipboard* clip = Clipboard())
                clip->SetText(SelectedText());
        }
        void CutSelection()
        {
            if (!HasSelection())
                return;
            if (IClipboard* clip = Clipboard())
                clip->SetText(SelectedText());
            DeleteSelection();
            NotifyChanged();
        }
        void Paste()
        {
            IClipboard* clip = Clipboard();
            if (clip == nullptr || !clip->HasText())
                return;
            const foundation::String text = clip->GetText();
            if (text.Size() == 0)
                return;
            InsertText(SanitizeSingleLine(text.AsView()).AsView());
        }

        // Remove any selection (no notify); caret collapses to the selection start.
        void DeleteSelection()
        {
            if (!HasSelection())
                return;
            const usize start = SelectionStart();
            const usize len = SelectionEnd() - start;
            m_value.Remove(start, len);
            m_caret = start;
            m_selAnchor = start;
            SyncText();
        }

        // Insert text at the caret, replacing any selection and honouring the max-length cap
        // (measured in codepoints). Fires the changed callback once.
        void InsertText(foundation::StringView text)
        {
            if (HasSelection())
                DeleteSelection();

            foundation::StringView toInsert = text;
            if (m_maxLength > 0)
            {
                const usize current = CountCodepoints(m_value.AsView());
                if (current >= m_maxLength)
                    return;
                const usize room = m_maxLength - current;
                toInsert = ClampCodepoints(text, room);
                if (toInsert.Size() == 0)
                    return;
            }

            m_value.Insert(m_caret, toInsert);
            m_caret += toInsert.Size();
            m_selAnchor = m_caret;
            SyncText();
            ResetBlink();
            NotifyChanged();
        }

        // Select the word (run of non-whitespace) surrounding a byte offset; an offset on
        // whitespace selects that whitespace run instead, so a click always selects something.
        void SelectWordAt(usize offset)
        {
            const foundation::StringView view = m_value.AsView();
            if (view.Size() == 0)
            {
                m_selAnchor = m_caret = 0;
                return;
            }
            offset = foundation::Min(offset, view.Size());

            // Decide the run kind from the codepoint to the right (or left, at the very end).
            usize probe = offset;
            if (probe >= view.Size())
                probe = foundation::Utf8PrevBoundary(view, view.Size());
            const bool wantSpace = foundation::IsWhiteSpace(view[probe]);

            usize start = offset;
            while (start > 0)
            {
                const usize prev = foundation::Utf8PrevBoundary(view, start);
                if (foundation::IsWhiteSpace(view[prev]) != wantSpace)
                    break;
                start = prev;
            }
            usize end = offset;
            while (end < view.Size())
            {
                if (foundation::IsWhiteSpace(view[end]) != wantSpace)
                    break;
                end = foundation::Utf8NextBoundary(view, end);
            }
            m_selAnchor = start;
            m_caret = end;
            Invalidate();
        }

        // Strip newlines so a pasted multi-line value stays on one line.
        static foundation::String SanitizeSingleLine(foundation::StringView text)
        {
            foundation::String out;
            for (usize i = 0; i < text.Size(); ++i)
            {
                const char8_t c = text[i];
                if (c == u8'\n' || c == u8'\r')
                    continue;
                out.Append(foundation::StringView(&c, 1));
            }
            return out;
        }

        [[nodiscard]] static usize CountCodepoints(foundation::StringView view)
        {
            usize count = 0;
            usize offset = 0;
            while (offset < view.Size())
            {
                offset = foundation::Utf8NextBoundary(view, offset);
                ++count;
            }
            return count;
        }

        // The prefix of `text` holding at most `maxCodepoints` codepoints.
        [[nodiscard]] static foundation::StringView ClampCodepoints(foundation::StringView text,
                                                              usize maxCodepoints)
        {
            usize offset = 0;
            usize count = 0;
            while (offset < text.Size() && count < maxCodepoints)
            {
                offset = foundation::Utf8NextBoundary(text, offset);
                ++count;
            }
            return text.SubStr(0, offset);
        }

        // Width of the value's prefix [0, byteOffset) in the current font (0 if no font).
        [[nodiscard]] f32 PrefixWidth(usize byteOffset) const
        {
            fonts::CachedFont* font = m_text.GetFont();
            if (font == nullptr || font->font == nullptr || byteOffset == 0)
                return 0.0f;
            return font->font->MeasureString(m_value.AsView().SubStr(0, byteOffset));
        }

        // The caret byte offset whose prefix width is closest to local x (within content).
        [[nodiscard]] usize CaretFromX(f32 localX) const
        {
            const Rect content = GetContentBounds();
            const f32 target = localX - content.x;
            if (target <= 0.0f)
                return 0;

            usize best = 0;
            f32 bestDist = target; // distance at offset 0 is |target - 0|
            usize offset = 0;
            const foundation::StringView view = m_value.AsView();
            while (offset < view.Size())
            {
                offset = foundation::Utf8NextBoundary(view, offset);
                const f32 w = PrefixWidth(offset);
                const f32 dist = (w >= target) ? (w - target) : (target - w);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    best = offset;
                }
            }
            return best;
        }

        foundation::String m_value;
        usize m_caret = 0;     // byte offset, codepoint boundary
        usize m_selAnchor = 0; // selection anchor; selection = [min,max) with caret
        Text m_text;

        foundation::String m_placeholder;
        Text m_hintText; // placeholder renderer (shares the field's font)
        Color m_placeholderColor{0.55f, 0.58f, 0.62f, 1.0f};
        usize m_maxLength = 0; // codepoints, 0 = unlimited

        Color m_caretColor{0.90f, 0.92f, 0.95f, 1.0f};
        Color m_selectionColor{0.25f, 0.45f, 0.85f, 0.60f};
        foundation::Function<void(foundation::StringView)> m_onChanged;

        bool m_selecting = false; // drag-select in progress (pointer captured)

        // Blink state + a monotonically increasing clock (advances via Update()); the clock
        // also times double-clicks.
        bool m_caretVisible = true;
        f64 m_blinkAccum = 0.0;
        f64 m_clock = 0.0;
        f64 m_lastClickTime = -1000.0; // far in the past: the first press is never a double
        f32 m_lastClickX = 0.0f;       // node-local x of the last press (double-click gate)

        static constexpr f64 kBlinkPeriod = 0.5;      // seconds per on/off half-cycle
        static constexpr f64 kDoubleClickTime = 0.4;  // max gap for a double-click
        static constexpr f32 kDoubleClickSlop = 4.0f; // max px between the two presses
        static constexpr f32 kCaretWidth = 1.5f;
    };

    DRACONIC_DEFINE_OBJECT(TextField, "draconic::gui")
}
