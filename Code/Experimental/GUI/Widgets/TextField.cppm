// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :text_field partition
//
// TextField: a single-line, editable text control over the editing model (E4 of
// Specs/gui-lab-evolution.md). Modeled on eepp's UITextInput (role only): typed text goes to the
// model, and every editing key reaches it as a command through the keymap, the field declaring the
// "textinput" key context and handling the editing commands' names (BindTextEditingKeys binds the
// keys), so a screen rebinds them and a menu shows them like any command. Its text is shaped and
// scrolls across to keep the caret in view; a mouse press places the caret, a drag selects, a double
// press selects a word and a triple the line.
//
// It reports each edit as `input` (SetOnTextChanging), and a committed value as `change`
// (SetOnTextChanged) when Return submits it (`submit`, SetOnSubmit) or focus leaves with a value
// other than the last committed one, as the web's inputs do. It may be read-only, filter what comes
// in, cap its length in graphemes, and mask its text as a password (copying and cutting then take
// nothing out). The caret blinks while it is focused.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:text_field;

import foundation.core;  // String, StringView, Color, Function, Move, Min, Max
import foundation.fonts; // CachedFont
import :reactive;        // Observable (what bindings follow)
import :rect;
import :draw_context;
import :text;
import :event;
import :clipboard;
import :event_dispatcher;
import :ui_widget;
import :actions; // TickAction (the caret's blink)
import :commands; // CommandEvent
import :text_segmentation;
import :input_filter;
import :text_edit_model;
import :text_editing_keys; // kTextSubmitCommand

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    class TextField : public UIWidget
    {
        RTTI_OBJECT(TextField, UIWidget)
    public:
        TextField()
        {
            SetTag(core::StringView(u8"textfield"));
            SetTabFocusable(true);
            SetKeyContext(core::StringView(u8"textinput"));
            m_text.SetWhiteSpace(TextWhiteSpace::Pre); // its spaces as typed, on one line
            m_text.SetAlignment(TextHAlign::Left, TextVAlign::Middle);
            m_hintText.SetAlignment(TextHAlign::Left, TextVAlign::Middle);
            m_hintText.SetColor(m_placeholderColor);
            // Each editing command, by the name the keymap binds, runs on its model.
            for (usize i = 0; i < static_cast<usize>(TextEditCommand::Count); ++i)
            {
                const TextEditCommand command = static_cast<TextEditCommand>(i);
                (void)OnCommand(TextEditCommandName(command), [this, command](CommandEvent&) { RunEdit(command); });
            }
            (void)OnCommand(core::StringView(kTextSubmitCommand), [this](CommandEvent&) { Submit(); });
        }

        // === Value ===
        /// Replaces the text from code: the caret goes to its end and the history goes; it is the
        /// committed value now, and reports no edit (as the web's value does).
        void SetText(core::StringView text)
        {
            if (text == m_model.GetText())
                return; // a binding writing back what was typed keeps the caret where it is
            m_model.SetText(text);
            m_committed = core::String(text);
            Synced();
            m_textChanged.Notify();
        }
        /// The text (a read a binding records).
        [[nodiscard]] core::StringView GetText() const
        {
            m_textChanged.Track();
            return m_model.GetText();
        }
        /// The editing model under it (its history, its rules).
        [[nodiscard]] TextEditModel& GetModel() noexcept { return m_model; }

        // Byte offset of the caret (always on a grapheme boundary).
        [[nodiscard]] usize GetCaret() const noexcept { return m_model.GetCaret(); }
        void SetCaret(usize byteOffset)
        {
            m_model.SetCaret(byteOffset);
            CaretMoved();
        }

        /// Each edit, with the text after it (the `input` event).
        void SetOnTextChanging(core::Function<void(core::StringView)> callback) { m_onChanging = core::Move(callback); }
        /// A committed value: Return, or focus leaving with a new one (the `change` event).
        void SetOnTextChanged(core::Function<void(core::StringView)> callback) { m_onChanged = core::Move(callback); }
        /// Return (the `submit` event), after the value commits.
        void SetOnSubmit(core::Function<void(core::StringView)> callback) { m_onSubmit = core::Move(callback); }

        // === Selection ===
        [[nodiscard]] bool HasSelection() const noexcept { return m_model.HasSelection(); }
        [[nodiscard]] usize SelectionStart() const noexcept { return m_model.GetSelection().Start; }
        [[nodiscard]] usize SelectionEnd() const noexcept { return m_model.GetSelection().End; }
        [[nodiscard]] core::StringView SelectedText() const { return m_model.SelectedText(); }
        void SelectAll()
        {
            m_model.SelectAll();
            CaretMoved();
        }
        void ClearSelection()
        {
            m_model.SetCaret(m_model.GetCaret());
            CaretMoved();
        }

        // === Placeholder, limits, rules ===
        // Hint text shown (dimmed) while the value is empty.
        void SetPlaceholder(core::StringView text)
        {
            m_placeholder = core::String(text);
            m_hintText.SetString(m_placeholder.AsView());
            SyncPlaceholderShown();
            Invalidate();
        }
        [[nodiscard]] core::StringView GetPlaceholder() const { return m_placeholder.AsView(); }
        void SetPlaceholderColor(Color color)
        {
            m_placeholderColor = color;
            m_hintText.SetColor(color);
            Invalidate();
        }

        /// A read-only field shows, selects and copies its value but takes no edits (CSS's
        /// :read-only; an editable one is :read-write).
        void SetReadOnly(bool readOnly)
        {
            if (readOnly == m_model.IsReadOnly())
                return;
            m_model.SetReadOnly(readOnly);
            MarkStyleStateDirty(StateReadOnly | StateReadWrite);
            Invalidate();
        }
        [[nodiscard]] bool IsReadOnly() const noexcept { return m_model.IsReadOnly(); }
        [[nodiscard]] u32 GetWidgetStates() const override
        {
            return (IsReadOnly() ? WidgetStateNone : WidgetStateEditable) |
                   (m_placeholderShown ? WidgetStatePlaceholderShown : WidgetStateNone);
        }

        /// The most graphemes it holds (0: no limit); what does not fit is cut.
        void SetMaxLength(usize graphemes) noexcept { m_model.SetMaxLength(graphemes); }
        [[nodiscard]] usize GetMaxLength() const noexcept { return m_model.GetMaxLength(); }
        /// What it lets in, typed or pasted.
        void SetInputFilter(InputFilter filter) { m_model.SetFilter(core::Move(filter)); }

        /// A password field draws a mask for each character and lets nothing out by copy or cut.
        void SetPassword(bool password)
        {
            if (password == m_password)
                return;
            m_password = password;
            m_model.SetCopyAllowed(!password);
            Synced();
        }
        [[nodiscard]] bool IsPassword() const noexcept { return m_password; }

        // An enabled TextField wants platform text input while focused (the bridge enables
        // the window's IME accordingly).
        [[nodiscard]] bool WantsTextInput() const override { return IsTreeEnabled(); }

        // === Appearance ===
        // Theming: the text colour drives the caret too (so it shows on any background); a
        // `textfield::caret` / `::selection` part can override either.
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"caret"));
            out.PushBack(core::StringView(u8"selection"));
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

        /// Whether the caret shows now (focused, and in the visible half of its blink).
        [[nodiscard]] bool IsCaretShown() const noexcept { return IsFocused() && m_caretVisible; }
        /// How far its text is scrolled across (to keep the caret in view).
        [[nodiscard]] f32 GetScrollX() const noexcept { return m_scrollX; }
        // Advances the caret's blink and the model's clock. A focused field in a GUI drives it
        // itself, on its root's clock; a field in no GUI shows a steady caret unless its owner
        // calls this.
        void Update(f64 deltaSeconds)
        {
            m_clock += deltaSeconds;
            m_model.SetTime(m_clock);
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

        /// One line of its font, a field's usual width, when nothing sizes it (E2).
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override
        {
            return core::Float2{160.0f, (m_text.GetFont() != nullptr) ? m_text.GetLineHeight() : 20.0f};
        }
        /// Its line's baseline, where it draws its value (and its placeholder, in the same font).
        [[nodiscard]] f32 BaselineAt(core::Float2 size) const override
        {
            const Thickness c = GetChrome();
            return m_text.FirstBaseline(Rect{c.Left, c.Top, core::Max(0.0f, size.x - c.TotalHorizontal()),
                                             core::Max(0.0f, size.y - c.TotalVertical())});
        }

    protected:
        // Its value and placeholder draw in its font (one line of it tall), its value and caret in
        // its colour.
        void OnTextStyleChange() override
        {
            m_text.SetFont(GetFont());
            m_hintText.SetFont(GetFont());
            m_text.SetColor(GetTextColor());
            m_text.SetLetterSpacing(GetTextStyle().LetterSpacing);
            m_caretColor = GetTextColor();
            KeepCaretInView();
        }
        void OnSizeChange() override { KeepCaretInView(); }

        void OnTextInput(const TextInputEvent& event) override
        {
            if (!IsTreeEnabled() || event.Text.Size() == 0)
                return;
            event.Handled = true;
            if (m_model.Insert(event.Text))
                Edited();
        }

        // The editing keys reach it as commands, through the keymap; a key that types (a letter, a
        // digit, the space bar) is its own, so it neither activates nor moves focus.
        void OnKeyDown(const KeyEvent& event) override
        {
            if (!IsTreeEnabled() || event.Target != this)
                return;
            if ((event.Modifiers & (KeyModCtrl | KeyModAlt | KeyModSuper)) != 0)
                return;
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            if (key == KeyCode::Space || (key >= KeyCode::A && key <= KeyCode::Num9))
                event.Handled = true;
        }

        void OnMouseDown(const MouseEvent& event) override
        {
            RequestFocus();
            event.Handled = true;
            // Only the primary button places the caret: a right press keeps the selection a context
            // menu would act on.
            if (event.Button != MouseButton::Left)
                return;
            const bool shift = (event.Modifiers & static_cast<u32>(KeyModShift)) != 0;
            const usize pos = OffsetAtPoint(ConvertToNodeSpace(event.Position));

            // A double press selects the word, a triple the whole line (the dispatcher counts them).
            if (event.ClickCount >= 2 && !shift)
            {
                if (event.ClickCount == 2 && !m_password)
                    m_model.SelectWordAt(pos);
                else
                    m_model.SelectAll(); // a password is one word
                m_selecting = false;
                CaretMoved();
                return;
            }
            m_model.SetCaret(pos, shift); // Shift extends from the anchor
            m_selecting = true;           // begin a drag-select (pointer is captured)
            CaretMoved();
        }

        void OnMouseMove(const MouseEvent& event) override
        {
            if (!m_selecting)
                return;
            m_model.SetCaret(OffsetAtPoint(ConvertToNodeSpace(event.Position)), true);
            CaretMoved();
        }

        void OnMouseUp(const MouseEvent& event) override
        {
            (void)event;
            m_selecting = false;
        }
        void OnPointerCancel(const MouseEvent&) override { m_selecting = false; }

        void OnFocusGained() override
        {
            ResetBlink();
            if (!m_blinking && GetActionManager() != nullptr)
            {
                // Blinks while focused: the tick ends itself when focus goes (and goes with the
                // field if the field does).
                m_blinking = true;
                RunAction(core::MakeRef<TickAction>(MemoryAllocator(), [this](core::Duration elapsed)
                {
                    if (!IsFocused())
                    {
                        m_blinking = false;
                        return false;
                    }
                    Update(elapsed.AsSecondsF());
                    return true;
                }));
            }
        }
        void OnFocusLost() override
        {
            m_caretVisible = false;
            m_selecting = false;
            Commit();
            Invalidate();
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect content = GetContentBounds();
            // What it shows stays inside its padding: a value or a placeholder longer than the
            // field is cut at its edges (the caret's width past the end, so a caret there shows).
            ctx.PushClip(Rect{content.x, 0.0f, content.width + kCaretWidth, GetSize().y});

            if (m_model.GetText().Size() == 0 && m_placeholder.Size() > 0)
            {
                m_hintText.Draw(ctx, content);
            }
            else
            {
                const TextLayout& layout = Layout();
                const core::Float2 origin = TextOrigin(content);
                // The selection sits behind the text.
                if (IsFocused() && HasSelection())
                {
                    const f32 x0 = origin.x + layout.CaretX(DisplayOffset(SelectionStart()));
                    const f32 x1 = origin.x + layout.CaretX(DisplayOffset(SelectionEnd()));
                    const f32 h = m_text.GetLineHeight();
                    const f32 top = content.y + content.height * 0.5f - h * 0.5f;
                    GetPartStyle(u8"selection").Draw(ctx, Rect{x0, top, x1 - x0, h}, FilledBox(m_selectionColor));
                }
                m_text.DrawLayout(ctx, layout, origin);
            }

            if (IsFocused() && m_caretVisible && !HasSelection())
            {
                const f32 caretX = TextOrigin(content).x + Layout().CaretX(DisplayOffset(GetCaret()));
                const f32 h = m_text.GetLineHeight();
                const f32 top = content.y + content.height * 0.5f - h * 0.5f;
                GetPartStyle(u8"caret").Draw(ctx, Rect{caretX, top, kCaretWidth, h}, FilledBox(m_caretColor));
            }
            ctx.PopClip();
        }

    private:
        // === The model's changes ===
        void RunEdit(TextEditCommand command)
        {
            if (!IsTreeEnabled())
                return;
            const u64 version = m_model.GetVersion();
            (void)m_model.Run(command, Clipboard());
            if (m_model.GetVersion() != version)
                Edited();
            else
                CaretMoved();
        }
        // An edit the user made: shown, followed, reported.
        void Edited()
        {
            Synced();
            ResetBlink();
            m_textChanged.Notify();
            if (m_onChanging)
                m_onChanging(m_model.GetText());
            SendEvent(Event(EventType::Input, this));
        }
        void CaretMoved()
        {
            KeepCaretInView();
            ResetBlink();
            Invalidate();
        }
        // The shown text after the model changed.
        void Synced()
        {
            if (m_password)
            {
                core::String mask;
                const core::StringView text = m_model.GetText();
                for (usize at = 0; at < text.Size(); at = NextGrapheme(text, at))
                    mask.Append(core::StringView(kMask));
                m_text.SetString(mask.AsView());
            }
            else
            {
                m_text.SetString(m_model.GetText());
            }
            SyncPlaceholderShown();
            KeepCaretInView();
            Invalidate();
        }
        // The value commits: reported once for each new value.
        void Commit()
        {
            if (m_model.GetText() == m_committed.AsView())
                return;
            m_committed = core::String(m_model.GetText());
            if (m_onChanged)
                m_onChanged(m_model.GetText());
            SendEvent(Event(EventType::Changed, this));
        }
        void Submit()
        {
            if (!IsTreeEnabled())
                return;
            Commit();
            if (m_onSubmit)
                m_onSubmit(m_model.GetText());
            SendEvent(Event(EventType::Submit, this));
        }

        // :placeholder-shown follows whether the value is empty under a placeholder.
        void SyncPlaceholderShown()
        {
            const bool shown = m_model.GetText().Size() == 0 && m_placeholder.Size() != 0;
            if (shown == m_placeholderShown)
                return;
            m_placeholderShown = shown;
            MarkStyleStateDirty(StatePlaceholderShown);
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

        // === Where things are ===
        [[nodiscard]] const TextLayout& Layout() const { return m_text.LayoutFor(Rect{}); } // one line, unbounded
        // The layout's origin in the field: its content's left less the scroll, its line centred.
        [[nodiscard]] core::Float2 TextOrigin(const Rect& content) const
        {
            const core::Float2 origin = m_text.Origin(Rect{content.x, content.y, 0.0f, content.height});
            return core::Float2{origin.x - m_scrollX, origin.y};
        }
        // A masked text's bytes for a value's offset, and back (each grapheme one mask).
        [[nodiscard]] usize DisplayOffset(usize offset) const
        {
            if (!m_password)
                return offset;
            return GraphemeCount(m_model.GetText().SubStr(0, offset)) * kMaskBytes;
        }
        [[nodiscard]] usize ModelOffset(usize display) const
        {
            if (!m_password)
                return display;
            const core::StringView text = m_model.GetText();
            usize at = 0;
            for (usize n = display / kMaskBytes; n > 0 && at < text.Size(); --n)
                at = NextGrapheme(text, at);
            return at;
        }
        [[nodiscard]] usize OffsetAtPoint(core::Float2 local) const
        {
            const core::Float2 origin = TextOrigin(GetContentBounds());
            return ModelOffset(Layout().OffsetAt(core::Float2{local.x - origin.x, local.y - origin.y}));
        }
        // Scrolls by the least that shows the caret, and no further than the text needs.
        void KeepCaretInView()
        {
            const f32 view = core::Max(0.0f, GetContentBounds().width - kCaretWidth);
            const TextLayout& layout = Layout();
            const f32 caret = layout.CaretX(DisplayOffset(GetCaret()));
            const f32 before = m_scrollX;
            if (caret - m_scrollX > view)
                m_scrollX = caret - view;
            if (caret < m_scrollX)
                m_scrollX = caret;
            m_scrollX = core::Max(0.0f, core::Min(m_scrollX, layout.Width - view));
            if (m_scrollX != before)
                Invalidate();
        }

        TextEditModel m_model;
        core::String m_committed; // the value last reported as changed
        Text m_text;              // what it shows: its value, or its mask
        Observable m_textChanged; // what bindings reading the text follow

        core::String m_placeholder;
        Text m_hintText; // placeholder renderer (shares the field's font)
        Color m_placeholderColor{0.55f, 0.58f, 0.62f, 1.0f};

        Color m_caretColor{0.90f, 0.92f, 0.95f, 1.0f};
        Color m_selectionColor{0.25f, 0.45f, 0.85f, 0.60f};
        core::Function<void(core::StringView)> m_onChanging;
        core::Function<void(core::StringView)> m_onChanged;
        core::Function<void(core::StringView)> m_onSubmit;

        f32 m_scrollX = 0.0f;
        bool m_selecting = false; // drag-select in progress (pointer is captured)
        bool m_password = false;

        // Blink state (advances via Update()), and the model's clock.
        bool m_caretVisible = true;
        f64 m_blinkAccum = 0.0;
        f64 m_clock = 0.0;

        bool m_blinking = false; // a blink tick is running
        bool m_placeholderShown = false;
        static constexpr f64 kBlinkPeriod = 0.5; // seconds per on/off half-cycle
        static constexpr f32 kCaretWidth = 1.5f;
        static constexpr const char8_t* kMask = u8"•"; // a bullet for each character
        static constexpr usize kMaskBytes = 3;
    };
}
