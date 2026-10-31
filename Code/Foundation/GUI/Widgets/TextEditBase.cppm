// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :text_edit_base partition
//
// TextEditBase: the base that the text field and the text area share. It holds the editing model.
// Typed text goes to the model, and every editing key reaches it as a command through the keymap:
// the widget declares the "textinput" key context and handles the editing commands by name
// (BindTextEditingKeys binds the keys). A screen can therefore rebind them, and a menu can show
// and run them like any other command. A mouse press places the caret, a drag selects, a double
// press selects a word and a triple press selects everything.
//
// Each edit sends `input` (SetOnTextChanging). A committed value sends `change`
// (SetOnTextChanged) when focus leaves with a value different from the last committed one, or
// when the widget submits (`submit`, SetOnSubmit), as web inputs do. A value set from code sends
// nothing. An IME composition shows in place, underlined over the selection it will replace, until
// it commits as typed text; the widget tells the host where it is typing, so the IME can place its
// candidates. The caret blinks while the widget is focused. A context menu (opened by a right
// release, a long press or Shift+F10) offers cut, copy, paste and select all, each enabled only
// when it would do something.
//
// A widget built on this base supplies the geometry: the layout it edits in (one line, or
// wrapped), where that layout sits (scrolled), and how it scrolls to keep the caret in view.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:text_edit_base;

import foundation.core;  // String, StringView, Color, Function, Move, Min, Max
import foundation.fonts; // CachedFont
import :reactive;        // Observable, which bindings follow
import :rect;
import :draw_context;
import :text;
import :event;
import :clipboard;
import :event_dispatcher;
import :ui_widget;
import :actions;  // TickAction, for the caret's blink
import :commands; // CommandEvent
import :text_segmentation;
import :input_filter;
import :text_edit_model;
import :text_editing_keys; // kTextSubmitCommand
import :menu; // the context menu
import :scene_root; // GUIContext, whose keymap gives the menu's shortcut text

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::gui
{
    class TextEditBase : public UIWidget
    {
        RTTI_OBJECT(TextEditBase, UIWidget)
    public:
        // === Value ===
        /// Replaces the text from code. The caret moves to the end, the undo history is cleared, and
        /// the text becomes the committed value. No edit is reported, as with a web input's value.
        void SetText(core::StringView text)
        {
            if (text == m_model.GetText())
                return; // when a binding writes back what was typed, the caret stays where it is
            m_model.SetText(text);
            m_committed = core::String(text);
            Synced();
            m_textChanged.Notify();
        }
        /// Returns the text. A binding that reads it follows later changes.
        [[nodiscard]] core::StringView GetText() const
        {
            m_textChanged.Track();
            return m_model.GetText();
        }
        /// Returns the underlying editing model, which holds the history and the input rules.
        [[nodiscard]] TextEditModel& GetModel() noexcept { return m_model; }
        /// Returns the context menu, or null until it is first used.
        [[nodiscard]] Menu* GetContextMenu() const noexcept { return m_contextMenu.Get(); }

        // Byte offset of the caret (always on a grapheme boundary).
        [[nodiscard]] usize GetCaret() const noexcept { return m_model.GetCaret(); }
        void SetCaret(usize byteOffset)
        {
            m_model.SetCaret(byteOffset);
            CaretMoved();
        }

        /// Sets the callback for each edit, given the text after it (the `input` event).
        void SetOnTextChanging(core::Function<void(core::StringView)> callback) { m_onChanging = core::Move(callback); }
        /// Sets the callback for a committed value: on submit, or when focus leaves with a new value
        /// (the `change` event).
        void SetOnTextChanged(core::Function<void(core::StringView)> callback) { m_onChanged = core::Move(callback); }
        /// Sets the callback for a submit (the `submit` event), run after the value commits.
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

        /// Sets whether the widget is read-only. A read-only widget shows, selects and copies its
        /// value but accepts no edits; it matches CSS's :read-only, and an editable one :read-write.
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

        /// Sets the most graphemes the text may hold (0 means no limit); text that does not fit is cut.
        void SetMaxLength(usize graphemes) noexcept { m_model.SetMaxLength(graphemes); }
        [[nodiscard]] usize GetMaxLength() const noexcept { return m_model.GetMaxLength(); }
        /// Sets the filter for what may be typed or pasted.
        void SetInputFilter(InputFilter filter) { m_model.SetFilter(core::Move(filter)); }
        /// Sets the filter by name, as markup and style sheets give it: "none", "digits" or
        /// "hex-digits".
        void SetInputFilterName(const core::String& name)
        {
            const core::StringView n = Trim(name.AsView());
            InputFilter filter;
            filter.SetMode(n == core::StringView(u8"digits")       ? InputFilterMode::Digits
                           : n == core::StringView(u8"hex-digits") ? InputFilterMode::HexDigits
                                                                   : InputFilterMode::None);
            SetInputFilter(core::Move(filter));
        }
        [[nodiscard]] core::String GetInputFilterName() const
        {
            switch (m_model.GetFilter().Mode())
            {
            case InputFilterMode::Digits: return core::String(u8"digits");
            case InputFilterMode::HexDigits: return core::String(u8"hex-digits");
            case InputFilterMode::Custom: return core::String(u8"custom");
            default: return core::String(u8"none");
            }
        }

        // An enabled widget wants platform text input while focused; the bridge enables the
        // window's IME to match.
        [[nodiscard]] bool WantsTextInput() const override { return IsTreeEnabled(); }
        /// Returns true for a masked field (a password), in which the platform's IME composes nothing.
        [[nodiscard]] bool IsTextInputSecret() const override { return IsMasked(); }

        // === Appearance ===
        // Styling: the text colour also colours the caret, so the caret shows on any background. A
        // `::caret` or `::selection` part can override either.
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

        /// Returns whether the caret shows now: focused, and in the visible half of its blink.
        [[nodiscard]] bool IsCaretShown() const noexcept { return IsFocused() && m_caretVisible; }
        // Advances the caret's blink and the model's clock. A focused widget in a GUI calls this
        // itself on its root's clock; a widget in no GUI shows a steady caret unless its owner
        // calls it.
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

        /// Returns where typing happens (the current line, from where a composition starts), so the
        /// IME can place its candidates.
        [[nodiscard]] bool GetTextInputArea(Rect& area, f32& caretX) const override
        {
            const TextLayout& layout = EditLayout();
            const core::Float2 origin = TextOrigin(EditBounds());
            const usize start = DisplayOffset(SelectionStart());
            const usize end = start + (Composing() ? m_composition.Size() : 0);
            const LaidOutLine line = LineAt(layout, DisplayCaret());
            const f32 left = origin.x + layout.CaretX(start);
            const f32 right = origin.x + layout.CaretX(end);
            area = Rect{left, origin.y + line.Top, core::Max(1.0f, right - left), line.Height};
            caretX = origin.x + layout.CaretX(DisplayCaret(), CaretUpstream());
            return true;
        }

    protected:
        TextEditBase()
        {
            SetTabFocusable(true);
            SetKeyContext(core::StringView(u8"textinput"));
            m_model.SetCaretPlaces(&m_places); // Left and Right move across the displayed text
            m_text.SetAlignment(TextHAlign::Start, TextVAlign::Top);
            m_hintText.SetAlignment(TextHAlign::Start, TextVAlign::Top);
            {
                StyleWriteScope defaults(*this); // `text-align` defaults to start; a style sheet may change it
                SetTextAlignment(TextHAlign::Start, TextVAlign::Top);
            }
            m_hintText.SetColor(m_placeholderColor);
            // Register each editing command under the name the keymap binds, running it on the model.
            for (usize i = 0; i < static_cast<usize>(TextEditCommand::Count); ++i)
            {
                const TextEditCommand command = static_cast<TextEditCommand>(i);
                (void)OnCommand(TextEditCommandName(command), [this, command](CommandEvent&) { RunEdit(command); });
            }
        }

        // === Supplied by a widget built on this base ===
        /// Returns the layout of the displayed text that editing works in.
        [[nodiscard]] virtual const TextLayout& EditLayout() const = 0;
        /// Returns where that layout's origin is in the widget, scrolled, for the content box
        /// `content`.
        [[nodiscard]] virtual core::Float2 TextOrigin(const Rect& content) const = 0;
        /// Scrolls the least amount that shows the caret.
        virtual void KeepCaretInView() = 0;
        /// Returns where the placeholder is drawn within `content`.
        [[nodiscard]] virtual Rect PlaceholderBounds(const Rect& content) const { return content; }
        /// Returns where the text goes: the content box, less any part of the widget beside the text
        /// (such as a number field's arrows).
        [[nodiscard]] virtual Rect EditBounds() const { return GetContentBounds(); }
        /// Returns whether the widget masks its text, as a password field does with a bullet per
        /// character.
        [[nodiscard]] virtual bool IsMasked() const { return false; }
        /// Called when the text is about to commit (on Return, or when focus leaves). A field that
        /// interprets its text (a number field's expression) settles it here; calling SetText makes
        /// its own text the committed value.
        virtual void OnCommitting() {}

        // === For a widget built on it ===
        [[nodiscard]] const Text& ShownText() const noexcept { return m_text; }
        Text& ShownText() noexcept { return m_text; }
        Text& HintText() noexcept { return m_hintText; }
        /// Returns the line that holds display offset `offset`.
        [[nodiscard]] static LaidOutLine LineAt(const TextLayout& layout, usize offset)
        {
            return layout.Lines.IsEmpty() ? LaidOutLine{} : layout.Lines[layout.LineOf(offset)];
        }
        /// Returns the display offset where the caret shows. During a composition it is at the
        /// composition's cursor, which counts characters.
        [[nodiscard]] usize DisplayCaret() const
        {
            if (!Composing())
                return DisplayOffset(GetCaret());
            const core::StringView composition = m_composition.AsView();
            usize at = 0;
            for (i32 n = 0; n < m_compositionCursor && at < composition.Size(); ++n)
                at = core::Utf8NextBoundary(composition, at);
            return SelectionStart() + at;
        }
        // Converts a value offset to a byte offset in the displayed text (and ModelOffset converts
        // back). With a mask, each grapheme shows as one mask string; during a composition, the
        // composition replaces the selection.
        [[nodiscard]] usize DisplayOffset(usize offset) const
        {
            if (IsMasked())
                return GraphemeCount(m_model.GetText().SubStr(0, offset)) * m_mask.Size();
            if (!Composing())
                return offset;
            const TextRange cut = m_model.GetSelection();
            if (offset <= cut.Start)
                return offset;
            if (offset >= cut.End)
                return offset - (cut.End - cut.Start) + m_composition.Size();
            return cut.Start;
        }
        [[nodiscard]] usize ModelOffset(usize display) const
        {
            if (IsMasked())
            {
                const core::StringView text = m_model.GetText();
                usize at = 0;
                for (usize n = display / m_mask.Size(); n > 0 && at < text.Size(); --n)
                    at = NextGrapheme(text, at);
                return at;
            }
            if (!Composing())
                return display;
            const TextRange cut = m_model.GetSelection();
            if (display <= cut.Start)
                return display;
            if (display <= cut.Start + m_composition.Size())
                return cut.Start;
            return display - m_composition.Size() + (cut.End - cut.Start);
        }
        [[nodiscard]] bool Composing() const noexcept { return m_composition.Size() != 0 && !IsMasked(); }
        /// Rebuilds the displayed text from the model's text after the text or the mask changed.
        void Synced()
        {
            if (Composing())
            {
                const core::StringView text = m_model.GetText();
                const TextRange cut = m_model.GetSelection();
                core::String shown(text.SubStr(0, cut.Start));
                shown.Append(m_composition.AsView());
                shown.Append(text.SubStr(cut.End, text.Size() - cut.End));
                m_text.SetString(shown.AsView());
            }
            else if (IsMasked())
            {
                core::String mask;
                const core::StringView text = m_model.GetText();
                for (usize at = 0; at < text.Size(); at = NextGrapheme(text, at))
                    mask.Append(m_mask.AsView());
                m_text.SetString(mask.AsView());
            }
            else
            {
                m_text.SetString(m_model.GetText());
            }
            SyncPlaceholderShown();
            ShowCaret();
            Invalidate();
        }
        /// Commits the value, then submits it (the `submit` event).
        void Submit()
        {
            if (!IsTreeEnabled())
                return;
            Commit();
            if (m_onSubmit)
                m_onSubmit(m_model.GetText());
            SendEvent(Event(EventType::Submit, this));
        }
        // Keeps the caret in view: first with the widget's own scroll, then with each scroll
        // container around it (a text area as tall as its text inside a scrolling box), as browsers
        // do while typing.
        void ShowCaret()
        {
            KeepCaretInView();
            Rect at;
            if (!IsFocused() || !GetFocusArea(at))
                return;
            for (Node* up = GetParent(); up != nullptr; up = up->GetParent())
                if (up->IsScrollContainer())
                    up->ScrollIntoView(*this, at, ScrollAlign::Nearest, ScrollBehavior::Instant);
        }
        // Returns the caret's area, which a surrounding scroll container keeps in view as the
        // content grows.
        [[nodiscard]] bool GetFocusArea(Rect& out) const override
        {
            const TextLayout& layout = EditLayout();
            const core::Float2 origin = TextOrigin(EditBounds());
            const usize caret = DisplayCaret();
            const LaidOutLine line = LineAt(layout, caret);
            const f32 h = line.Height > 0.0f ? line.Height : m_text.GetLineHeight();
            out = Rect{origin.x + layout.CaretX(caret, CaretUpstream()), origin.y + line.Top, kCaretWidth, h};
            return true;
        }
        void CaretMoved()
        {
            ShowCaret();
            ResetBlink();
            Invalidate();
        }

        // The value and the placeholder use the widget's font; the value and the caret use its colour.
        void OnTextStyleChange() override
        {
            m_text.SetFont(GetFont());
            m_text.SetFontSelection(GetFontService(), GetTextStyle().FontFamily.AsView(), GetTextStyle().Selection);
            m_hintText.SetFont(GetFont());
            m_hintText.SetFontSelection(GetFontService(), GetTextStyle().FontFamily.AsView(), GetTextStyle().Selection);
            m_text.SetDirection(TextDirectionFor());
            m_hintText.SetDirection(TextDirectionFor());
            // Horizontally, as `text-align` says; vertically, as the editor places its lines.
            m_text.SetAlignment(GetTextAlignH(), m_text.GetVAlign());
            m_hintText.SetAlignment(GetTextAlignH(), m_hintText.GetVAlign());
            m_text.SetColor(GetTextColor());
            m_text.SetLetterSpacing(GetTextStyle().LetterSpacing);
            m_text.SetBreakWords(GetTextStyle().BreakWords);
            m_text.SetLineHeight(GetTextStyle().LineHeightPixels());
            m_hintText.SetLineHeight(GetTextStyle().LineHeightPixels());
            m_caretColor = GetTextColor();
            // The password mark is a bullet, or a middle dot when the font has no bullet.
            m_mask = core::String(core::StringView(FontDraws(GetFont(), core::StringView(kBullet)) ? kBullet : kMiddleDot));
            if (IsMasked())
                Synced();
            KeepCaretInView();
        }
        void OnSizeChange() override { ShowCaret(); }

        void OnTextInput(const TextInputEvent& event) override
        {
            if (!IsTreeEnabled() || event.Text.Size() == 0)
                return;
            event.Handled = true;
            m_composition.Clear(); // the composition commits as this text
            if (m_model.Insert(event.Text))
                Edited();
            else
                Synced();
        }
        // Handles the IME's composition so far: it shows in place of the selection, with the caret
        // inside it.
        void OnComposition(const CompositionEvent& event) override
        {
            if (!IsTreeEnabled() || IsReadOnly() || IsMasked())
                return;
            event.Handled = true;
            m_composition = core::String(event.Text);
            m_compositionCursor = core::Max(0, event.Cursor);
            Synced();
            ResetBlink();
        }

        // The editing keys arrive as commands through the keymap. A key that types (a letter, a
        // digit, the space bar) is handled here, so it neither activates anything nor moves focus.
        void OnKeyDown(const KeyEvent& event) override
        {
            if (!IsTreeEnabled() || event.Target != this)
                return;
            if ((event.Modifiers & (KeyModCtrl | KeyModAlt | KeyModSuper)) != 0)
                return;
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            if (key == KeyCode::Space || (key >= KeyCode::A && key <= KeyCode::Num9))
                event.Handled = true;
            // When the widget captures Tab, Tab types a tab character, since no platform sends Tab
            // as text.
            if (key == KeyCode::Tab && event.Modifiers == 0 && GetTabCaptures() && !IsReadOnly())
            {
                event.Handled = true;
                m_composition.Clear();
                if (m_model.Insert(core::StringView(u8"\t")))
                    Edited();
            }
        }

        // Opens the context menu at the event's position with cut, copy, paste and select all, each
        // enabled only when it would do something.
        void OnContextMenu(const MouseEvent& event) override
        {
            event.Handled = true;
            if (!m_contextMenu)
            {
                m_contextMenu = core::MakeRef<Menu>(MemoryAllocator());
                m_contextMenu->SetAutoWidth(true);
                TextEditBase* self = this;
                // Each label is translated (gui.cut, ...) where the GUI's tables have the key,
                // otherwise English.
                const auto add = [&](const char8_t* label, const char8_t* key, TextEditCommand command)
                {
                    MenuItem* item = m_contextMenu->AddItem(core::StringView(label), [self, command] { self->RunEdit(command); });
                    (void)item->BindTranslation(core::StringView(u8"text"), core::StringView(key), {}, {}, {}, core::StringView(label));
                    m_contextItems.PushBack(item);
                };
                add(u8"Cut", u8"gui.cut", TextEditCommand::Cut);
                add(u8"Copy", u8"gui.copy", TextEditCommand::Copy);
                add(u8"Paste", u8"gui.paste", TextEditCommand::Paste);
                m_contextMenu->AddSeparator();
                add(u8"Select all", u8"gui.select-all", TextEditCommand::SelectAll);
            }
            const bool selected = HasSelection() && m_model.IsCopyAllowed();
            IClipboard* clipboard = Clipboard();
            m_contextItems[0]->SetEnabled(selected && !IsReadOnly());
            m_contextItems[1]->SetEnabled(selected);
            m_contextItems[2]->SetEnabled(!IsReadOnly() && clipboard != nullptr && clipboard->HasText());
            m_contextItems[3]->SetEnabled(m_model.GetText().Size() != 0);
            // Each item shows the keys the GUI binds to its command ("Ctrl+X").
            static constexpr TextEditCommand kCommands[] = {TextEditCommand::Cut, TextEditCommand::Copy, TextEditCommand::Paste,
                                                            TextEditCommand::SelectAll};
            if (SceneRoot* scene = GetSceneRoot())
            {
                // Use the keys bound in the field's own contexts ("textinput"), which are the ones
                // that work here.
                const Array<KeyContextLayer> contexts = scene->KeyContextsAt(this);
                for (usize i = 0; i < m_contextItems.Size(); ++i)
                    m_contextItems[i]->SetShortcut(
                        scene->GetContext().GetKeymap().ShortcutText(TextEditCommandName(kCommands[i]), contexts.AsSpan()).AsView());
            }
            Node* root = GetRootNode();
            m_contextMenu->Open(*this, root != nullptr ? root->ConvertToNodeSpace(event.Position) : event.Position);
        }

        // A press selects text, so a drag is never offered to the nodes that hold the widget.
        [[nodiscard]] bool OwnsPressDrag(core::Float2) const override { return true; }
        void OnMouseDown(const MouseEvent& event) override
        {
            RequestFocus();
            event.Handled = true;
            // Only the primary button places the caret. A right press keeps the selection, which a
            // context menu would act on.
            if (event.Button != MouseButton::Left)
                return;
            const bool shift = (event.Modifiers & static_cast<u32>(KeyModShift)) != 0;
            bool upstream = false;
            const usize pos = OffsetAtPoint(ConvertToNodeSpace(event.Position), &upstream);

            // A double press selects the word and a triple press everything; the dispatcher counts
            // the presses.
            if (event.ClickCount >= 2 && !shift)
            {
                if (event.ClickCount == 2 && !IsMasked())
                    m_model.SelectWordAt(pos);
                else
                    m_model.SelectAll(); // a password counts as one word
                m_selecting = false;
                CaretMoved();
                return;
            }
            m_model.SetCaret(pos, shift, upstream); // Shift extends from the anchor
            m_selecting = true;           // begin a drag-select (the pointer is captured)
            CaretMoved();
        }

        void OnMouseMove(const MouseEvent& event) override
        {
            if (!m_selecting)
                return;
            bool upstream = false;
            const usize pos = OffsetAtPoint(ConvertToNodeSpace(event.Position), &upstream);
            m_model.SetCaret(pos, true, upstream);
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
                // Blink while focused. The tick ends itself when focus leaves, and is destroyed with
                // the widget if the widget goes.
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
            if (m_composition.Size() != 0)
            {
                m_composition.Clear(); // an unfinished composition is dropped when focus leaves
                Synced();
            }
            Commit();
            KeepCaretInView(); // unfocused, the view returns to the start of the line
            Invalidate();
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect content = EditBounds();
            // Clip the drawing to the padding box, widened by the caret's width on the right so a
            // caret at the end still shows.
            ctx.PushClip(Rect{content.x, content.y, content.width + kCaretWidth, content.height});

            if (m_model.GetText().Size() == 0 && m_placeholder.Size() > 0 && !Composing())
            {
                m_hintText.Draw(ctx, PlaceholderBounds(content));
            }
            else
            {
                const TextLayout& layout = EditLayout();
                const core::Float2 origin = TextOrigin(content);
                // Draw the selection behind the text, unless a composition replaces it.
                if (IsFocused() && HasSelection() && !Composing())
                    DrawRange(ctx, layout, origin, DisplayOffset(SelectionStart()), DisplayOffset(SelectionEnd()), false);
                m_text.DrawLayout(ctx, layout, origin);
                if (Composing())
                {
                    // Underline the composition, as IMEs show text that is not yet committed.
                    const usize start = DisplayOffset(SelectionStart());
                    DrawRange(ctx, layout, origin, start, start + m_composition.Size(), true);
                }
            }

            if (IsFocused() && m_caretVisible && (!HasSelection() || Composing()))
            {
                const TextLayout& layout = EditLayout();
                const core::Float2 origin = TextOrigin(content);
                const usize caret = DisplayCaret();
                const LaidOutLine line = LineAt(layout, caret);
                const f32 h = line.Height > 0.0f ? line.Height : m_text.GetLineHeight();
                GetPartStyle(u8"caret").Draw(ctx, Rect{origin.x + layout.CaretX(caret, CaretUpstream()), origin.y + line.Top, kCaretWidth, h},
                                             FilledBox(m_caretColor));
            }
            ctx.PopClip();
        }

        static constexpr f32 kCaretWidth = 1.5f;

    private:
        // Draws a range of the displayed text line by line, either as the selection behind the
        // text or as a composition's underline.
        void DrawRange(DrawContext& ctx, const TextLayout& layout, core::Float2 origin, usize start, usize end, bool underline) const
        {
            for (usize i = 0; i < layout.Lines.Size(); ++i)
            {
                const LaidOutLine& line = layout.Lines[i];
                const usize a = core::Max(start, line.Start);
                const usize b = core::Min(end, line.End);
                // A range that continues past a line ending at a break includes the break.
                const bool wraps = i + 1 < layout.Lines.Size() && layout.Lines[i + 1].Start == line.End;
                const bool takesBreak = !wraps && end > line.End && i + 1 < layout.Lines.Size();
                if (a > b || (a == b && !takesBreak))
                    continue;
                // Draw in pieces where text directions mix; an included break shows past the line's
                // end.
                layout.RangeOnLine(a, b, i, m_rangeBoxes);
                if (takesBreak && !underline)
                {
                    const f32 lineEnd = layout.CaretXOnLine(line.End, i, true);
                    m_rangeBoxes.PushBack(line.RightToLeft ? core::Float2{lineEnd - 4.0f, lineEnd} : core::Float2{lineEnd, lineEnd + 4.0f});
                }
                for (const core::Float2 box : m_rangeBoxes)
                {
                    if (underline)
                        ctx.VG().FillRect(Rectangle{origin.x + box.x, origin.y + line.Top + line.Height - 1.0f, box.y - box.x, 1.0f},
                                          m_text.GetColor());
                    else
                        GetPartStyle(u8"selection").Draw(ctx, Rect{origin.x + box.x, origin.y + line.Top, box.y - box.x, line.Height},
                                                         FilledBox(m_selectionColor));
                }
            }
        }
        // Returns whether the caret attaches to the text before it (the model's affinity); never
        // while composing.
        [[nodiscard]] bool CaretUpstream() const { return !Composing() && m_model.IsCaretUpstream(); }

        // Caret positions in the displayed text. They come from the layout, with offsets mapped
        // through the mask or the composition.
        class ShownCaretPlaces : public ITextCaretPlaces
        {
        public:
            explicit ShownCaretPlaces(TextEditBase& edit) : m_edit(edit) {}
            [[nodiscard]] bool OffsetVisually(usize offset, bool upstream, i32 step, usize& outOffset, bool& outUpstream) const override
            {
                const TextLayout& layout = m_edit.EditLayout();
                if (layout.GlyphRuns.IsEmpty())
                {
                    // Nothing is shaped yet (no font), so move one grapheme in byte order.
                    const core::StringView text = m_edit.m_model.GetText();
                    outOffset = step < 0 ? PrevGrapheme(text, offset) : NextGrapheme(text, offset);
                    outUpstream = false;
                    return true;
                }
                usize display = 0;
                if (!layout.OffsetVisually(m_edit.DisplayOffset(offset), upstream, step, display, outUpstream))
                    return false;
                outOffset = m_edit.ModelOffset(display);
                return true;
            }
            [[nodiscard]] bool IsRightToLeftAt(usize offset) const override
            {
                const TextLayout& layout = m_edit.EditLayout();
                return !layout.Lines.IsEmpty() && layout.Lines[layout.LineOf(m_edit.DisplayOffset(offset))].RightToLeft;
            }

        private:
            TextEditBase& m_edit;
        };

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
        // Handles an edit the user made: updates the display, notifies bindings and sends `input`.
        void Edited()
        {
            Synced();
            ResetBlink();
            m_textChanged.Notify();
            if (m_onChanging)
                m_onChanging(m_model.GetText());
            SendEvent(Event(EventType::Input, this));
        }
        // Commits the value, reporting `change` once for each new value.
        void Commit()
        {
            OnCommitting();
            if (m_model.GetText() == m_committed.AsView())
                return;
            m_committed = core::String(m_model.GetText());
            if (m_onChanged)
                m_onChanged(m_model.GetText());
            SendEvent(Event(EventType::Changed, this));
        }

        // Keeps :placeholder-shown in step with whether the value is empty and a placeholder is set.
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

        // Returns the dispatcher's clipboard adapter, or null when no clipboard is configured.
        [[nodiscard]] IClipboard* Clipboard()
        {
            EventDispatcher* dispatcher = GetEventDispatcher();
            return dispatcher != nullptr ? dispatcher->GetClipboard() : nullptr;
        }

        [[nodiscard]] usize OffsetAtPoint(core::Float2 local, bool* upstream = nullptr) const
        {
            const core::Float2 origin = TextOrigin(EditBounds());
            return ModelOffset(EditLayout().OffsetAt(core::Float2{local.x - origin.x, local.y - origin.y}, upstream));
        }

        core::RefPtr<Menu> m_contextMenu;  // cut, copy, paste, select all; a child of the widget while open
        Array<MenuItem*> m_contextItems;   // non-owning; the menu owns the items
        TextEditModel m_model;
        core::String m_committed;    // the value last reported as changed
        core::String m_composition;  // an IME's text not yet committed
        i32 m_compositionCursor = 0; // the composition's caret, in characters
        Text m_text;                 // the displayed text: the value, or its mask
        Observable m_textChanged; // notifies bindings that read the text

        core::String m_placeholder;
        Text m_hintText; // placeholder renderer (shares the widget's font)
        Color m_placeholderColor{0.55f, 0.58f, 0.62f, 1.0f};

        Color m_caretColor{0.90f, 0.92f, 0.95f, 1.0f};
        Color m_selectionColor{0.25f, 0.45f, 0.85f, 0.60f};
        core::Function<void(core::StringView)> m_onChanging;
        core::Function<void(core::StringView)> m_onChanged;
        core::Function<void(core::StringView)> m_onSubmit;

        bool m_selecting = false; // drag-select in progress (pointer is captured)
        ShownCaretPlaces m_places{*this};
        mutable Array<core::Float2> m_rangeBoxes; // the boxes of a range on one line, reused each draw

        // Blink state (advances via Update()), and the model's clock.
        bool m_caretVisible = true;
        f64 m_blinkAccum = 0.0;
        f64 m_clock = 0.0;

        bool m_blinking = false; // a blink tick is running
        bool m_placeholderShown = false;
        static constexpr f64 kBlinkPeriod = 0.5; // seconds per on/off half-cycle
        static constexpr const char8_t* kBullet = u8"\u2022";
        static constexpr const char8_t* kMiddleDot = u8"\u00B7";
        core::String m_mask{core::StringView(kBullet)}; // a mark for each character of a password
    };
}
