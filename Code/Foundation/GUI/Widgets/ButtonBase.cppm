// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :button_base partition
//
// ButtonBase is the core that every button is built on. It activates on a click, Return, Space or
// the pad's South button. `toggle` makes it hold a checked state (`:checked`). `action-on="press"`
// activates it on the press instead of the release. `repeat` activates it again while it is held:
// after a delay, then at an interval on the GUI's clock, and only while the pointer is over it. A
// group makes its toggles exclusive. A group is a ButtonGroup from code, or a `name` in markup whose
// scope is the GUI the button is in, so groups of the same name in two windows never meet. The
// arrow keys move round an exclusive group, checking as they go, as a browser's radio buttons do.
//
// A button is a container. `text` and an icon are shorthands for parts it makes: a text node and an
// image node, which take the button's text style, as Window's title does. Markup may put anything
// inside it. CheckBox, RadioButton and ToggleSwitch are this core with an indicator part, a node the
// button draws from its own parts (checkbox::box and ::mark, and so on).
//
// A change the user makes reports `change` (and calls the toggled callback). A state set from code
// is reported only to bindings, as a range's value is.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:button_base;

import foundation.core; // Function, Move, RefPtr, Array, String, Duration, Max
import :reactive;       // Observable, which bindings follow
import :rect;
import :event;
import :draw_context;
import :drawable;
import :text;           // TextHAlign, TextVAlign
import :ui_widget;
import :label;
import :image;
import :layout_style;
import :actions;        // TickAction, for the repeat
import :event_dispatcher; // SuppressClick, after a press that activated

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class ButtonBase;

    /// When a button activates: on the release over it (a click), or on the press.
    enum class ButtonAction : u8
    {
        Release,
        Press,
    };

    /// An exclusive group of toggle buttons, made from code (markup groups them by a `name`). At
    /// most one is checked, and unless the group allows none, a click on the checked one leaves it
    /// checked. The buttons hold the group, so it outlives them, and each leaves it before it goes
    /// away.
    class ButtonGroup : public RefCounted
    {
    public:
        /// Sets whether a click on the checked button unchecks it.
        void SetAllowsNone(bool allows) noexcept { m_allowsNone = allows; }
        [[nodiscard]] bool AllowsNone() const noexcept { return m_allowsNone; }

        /// Returns the buttons in it, in the order they joined.
        [[nodiscard]] const core::Array<ButtonBase*>& GetButtons() const noexcept { return m_buttons; }
        /// Returns the checked button, or null.
        [[nodiscard]] ButtonBase* GetChecked() const;

        /// Called when the user checks `button` (not for changes from code).
        void SetOnChanged(core::Function<void(ButtonBase*)> callback) { m_onChanged = core::Move(callback); }

    private:
        friend class ButtonBase;
        void Join(ButtonBase* button) { m_buttons.PushBack(button); }
        void Leave(ButtonBase* button)
        {
            for (usize i = 0; i < m_buttons.Size(); ++i)
                if (m_buttons[i] == button)
                {
                    m_buttons.RemoveAt(i);
                    return;
                }
        }

        core::Array<ButtonBase*> m_buttons; // non-owning: each button leaves before it goes away
        bool m_allowsNone = false;
        core::Function<void(ButtonBase*)> m_onChanged;
    };

    // A button's indicator (a check box's box, a radio's ring, a switch's track). It is a node of its
    // own, laid out among the button's children and sized by a sheet as `checkbox > indicator`, and
    // its button draws it from the button's parts.
    class ButtonIndicator : public UIWidget
    {
        RTTI_OBJECT(ButtonIndicator, UIWidget)
    public:
        explicit ButtonIndicator(ButtonBase& owner);
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override;

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override;

    private:
        ButtonBase& m_owner; // its parent, which owns it
    };

    class ButtonBase : public UIWidget
    {
        RTTI_OBJECT(ButtonBase, UIWidget)
        friend class ButtonIndicator;
    public:
        ~ButtonBase() override
        {
            if (m_group)
                m_group->Leave(this);
        }

        // === Activation ===
        /// Sets the callback invoked on each activation (a click, a key, a repeat).
        void SetOnClick(core::Function<void()> callback) { m_onClick = core::Move(callback); }
        [[nodiscard]] bool HasOnClick() const noexcept { return static_cast<bool>(m_onClick); }

        void SetActionOn(ButtonAction action) noexcept { m_actionOn = action; }
        [[nodiscard]] ButtonAction GetActionOn() const noexcept { return m_actionOn; }
        /// Sets when it activates, "release" or "press" (the `action-on` of markup and sheets).
        void SetActionOnName(const core::String& name)
        {
            m_actionOn = name.AsView() == core::StringView(u8"press") ? ButtonAction::Press : ButtonAction::Release;
        }
        [[nodiscard]] core::String GetActionOnName() const
        {
            return core::String(m_actionOn == ButtonAction::Press ? core::StringView(u8"press") : core::StringView(u8"release"));
        }

        /// Sets whether it activates again while held: on the press, then after the delay, then at
        /// the interval.
        void SetRepeat(bool repeat) noexcept { m_repeat = repeat; }
        [[nodiscard]] bool IsRepeat() const noexcept { return m_repeat; }
        void SetRepeatDelay(f32 seconds) noexcept { m_repeatDelay = core::Max(0.0f, seconds); }
        [[nodiscard]] f32 GetRepeatDelay() const noexcept { return m_repeatDelay; }
        void SetRepeatInterval(f32 seconds) noexcept { m_repeatInterval = core::Max(0.01f, seconds); }
        [[nodiscard]] f32 GetRepeatInterval() const noexcept { return m_repeatInterval; }

        // === Toggle ===
        void SetToggle(bool toggle)
        {
            if (toggle == m_toggle)
                return;
            m_toggle = toggle;
            if (!m_toggle)
                ApplyChecked(false);
        }
        [[nodiscard]] bool IsToggle() const noexcept { return m_toggle; }
        /// Returns its access key, the character its text marked with `&`.
        [[nodiscard]] char32_t GetMnemonic() const override { return m_mnemonic; }
        /// A checked button that keeps its check (a radio) is the current member of its focus group.
        [[nodiscard]] bool IsCurrentInGroup() const override { return m_checked && KeepsChecked(); }

        /// Returns whether it is checked; a binding that reads this follows the state.
        [[nodiscard]] bool IsChecked() const
        {
            m_checkedChanged.Track();
            return m_checked;
        }
        /// Sets the checked state from code. The other buttons of an exclusive group clear, and no
        /// events fire.
        void SetChecked(bool checked)
        {
            if (m_toggle || !checked)
                ApplyChecked(checked);
        }
        /// Called on each toggle the user makes (not for changes from code).
        void SetOnToggled(core::Function<void(bool)> callback) { m_onToggled = core::Move(callback); }

        // === Group ===
        /// Joins `group`, or leaves its group with null. A group's toggles are exclusive.
        void SetGroup(core::RefPtr<ButtonGroup> group)
        {
            if (group == m_group)
                return;
            if (m_group)
                m_group->Leave(this);
            m_group = core::Move(group);
            if (m_group)
                m_group->Join(this);
        }
        [[nodiscard]] ButtonGroup* GetGroup() const noexcept { return m_group.Get(); }
        /// Sets its group's name in markup; buttons with the same name in one GUI form a group.
        void SetName(const core::String& name) { m_name = name; }
        [[nodiscard]] core::String GetName() const { return m_name; }
        /// Fills `out` with the buttons of its group, itself included, in tree order for a named
        /// group. It adds none when the button is ungrouped.
        void CollectGroup(core::Array<ButtonBase*>& out) const
        {
            if (m_group)
            {
                for (ButtonBase* b : m_group->GetButtons())
                    out.PushBack(b);
                return;
            }
            if (m_name.IsEmpty())
                return;
            const Node* top = this;
            while (top->GetParent() != nullptr)
                top = top->GetParent();
            CollectNamed(*top, out);
        }

        // === Content ===
        /// Sets its text, a part it makes after its icon; empty removes it.
        void SetText(core::StringView text)
        {
            m_textFromCommand = false;
            ApplyText(text);
        }
        [[nodiscard]] core::StringView GetText() const
        {
            m_textChanged.Track();
            return m_textPart ? m_textPart->GetText() : core::StringView{};
        }
        /// Sets its icon, an image part before its text; null removes it.
        void SetIcon(core::RefPtr<Drawable> icon)
        {
            if (!icon)
            {
                if (m_iconPart)
                {
                    m_iconPart->RemoveFromParent();
                    m_iconPart = core::RefPtr<Image>{};
                }
                return;
            }
            EnsureIconPart();
            m_iconPart->SetDrawable(core::Move(icon));
        }
        /// Sets its icon by path (markup's `icon`), loaded through the GUI's resource provider; empty
        /// removes it.
        void SetIconSource(const core::String& path)
        {
            m_iconSource = path;
            ShowIconSource();
        }
        [[nodiscard]] core::String GetIconSource() const { return m_iconSource; }
        /// Sets a toggle's icon for while it is checked (markup's `checked-icon`), such as a play
        /// button that shows pause while playing, since a click then pauses. Empty uses its icon in
        /// both states.
        void SetCheckedIconSource(const core::String& path)
        {
            m_checkedIconSource = path;
            ShowIconSource();
        }
        [[nodiscard]] core::String GetCheckedIconSource() const { return m_checkedIconSource; }
        /// Returns the parts it made, or null until it makes them.
        [[nodiscard]] Label* GetTextPart() const noexcept { return m_textPart.Get(); }
        [[nodiscard]] Image* GetIconPart() const noexcept { return m_iconPart.Get(); }
        [[nodiscard]] Node* GetIndicatorPart() const noexcept { return m_indicator.Get(); }

        // Markup: <Button text="Save"> makes its text a part.
        bool SetMarkupAttribute(core::StringView name, core::StringView value) override
        {
            if (name == core::StringView(u8"text"))
            {
                SetText(value);
                return true;
            }
            return UIWidget::SetMarkupAttribute(name, value);
        }
        /// Returns its text's first baseline, so a row aligned by baseline lines buttons up with
        /// labels. The text part sits centred across its content box.
        [[nodiscard]] f32 BaselineAt(core::Float2 size) const override
        {
            if (!m_textPart || !m_textPart->IsDisplayed())
                return UIWidget::BaselineAt(size);
            const Thickness c = GetChrome();
            const Thickness pc = m_textPart->GetChrome();
            // Use its arranged height, or one line's height before it is arranged.
            const f32 partH = m_textPart->GetSize().y > 0.0f ? m_textPart->GetSize().y
                                                             : m_textPart->GetTextLayout().GetLineHeight() + pc.TotalVertical();
            const f32 contentH = core::Max(0.0f, size.y - c.TotalVertical());
            return c.Top + (contentH - partH) * 0.5f + m_textPart->BaselineAt(core::Float2{m_textPart->GetSize().x, partH});
        }

        // A button with no text of its own shows its command's label.
        void OnBoundCommandText(core::StringView label, core::StringView, core::StringView) override
        {
            if (GetText().Size() != 0 && !m_textFromCommand)
                return;
            ApplyText(label);
            m_textFromCommand = true;
        }
        void OnBoundCommandState(const BoundCommandState& state) override
        {
            SetEnabled(state.Available);
            if (m_toggle)
                ApplyChecked(state.Checked);
        }

        [[nodiscard]] u32 GetWidgetStates() const override
        {
            return (m_checked ? WidgetStateChecked : WidgetStateNone) | (m_indeterminate ? WidgetStateIndeterminate : WidgetStateNone);
        }

    protected:
        ButtonBase()
        {
            SetTabFocusable(true);
            StyleWriteScope defaults(*this); // set as its own defaults, which a restyle returns to
            SetTextAlignment(TextHAlign::Center, TextVAlign::Middle);
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Row;
                s.AlignItems = Align::Center;
                s.ColumnGap = 6.0f;
            });
        }

        // --- The indicator (a check box's box, a radio's ring, a switch's track) ---
        /// Makes the indicator part, first among its children; the button draws it.
        void MakeIndicator()
        {
            if (m_indicator)
                return;
            m_indicator = core::MakeRef<ButtonIndicator>(MemoryAllocator(), *this);
            AddChildAt(m_indicator.Get(), 0);
        }
        /// The indicator's size when nothing sizes it.
        [[nodiscard]] virtual core::Float2 IndicatorSize() const { return core::Float2{20.0f, 20.0f}; }
        /// Draws the indicator into `box` (the indicator's content box, in its space) from the
        /// button's parts.
        virtual void DrawIndicator(DrawContext& ctx, const Rect& box) const
        {
            (void)ctx;
            (void)box;
        }
        /// Sets radio behaviour: a click on it while checked leaves it checked, grouped or not.
        void SetKeepsChecked(bool keeps) noexcept { m_keepsChecked = keeps; }
        /// Sets the mixed state (CSS's :indeterminate), which a check box shows until it is set.
        void SetIndeterminateState(bool indeterminate)
        {
            if (indeterminate == m_indeterminate)
                return;
            m_indeterminate = indeterminate;
            MarkStyleStateDirty(StateIndeterminate);
            RedrawIndicator();
        }
        [[nodiscard]] bool IsIndeterminateState() const noexcept { return m_indeterminate; }
        /// Called when the checked state changed (a switch moves its knob).
        virtual void OnCheckedChange() {}

        // --- Activation ---
        /// Performs one activation: a toggle toggles (unless an exclusive group keeps it checked), a
        /// bound command runs, and the click callback is invoked.
        void Activate()
        {
            if (m_toggle && !HasCommand())
            {
                const bool keep = m_checked && !m_indeterminate && KeepsChecked();
                if (!keep)
                    ChangeCheckedByUser(m_indeterminate ? true : !m_checked);
            }
            if (HasCommand())
                (void)RunBoundCommand(); // a bound command decides, and its state comes back to the button
            if (m_onClick)
                m_onClick();
        }

        bool OnActivate() override { return ActivateAsClick(); } // Return, Space and South act as a click
        void OnMouseClick(const MouseEvent& event) override
        {
            event.Handled = true; // a click inside a clickable card is the button's, not the card's
            Activate();
        }
        void OnMouseDown(const MouseEvent& event) override
        {
            if (event.Button != MouseButton::Left || (m_actionOn != ButtonAction::Press && !m_repeat))
                return;
            event.Handled = true;
            // It activates now, so the release must not make a second click.
            if (EventDispatcher* d = GetEventDispatcher())
                d->SuppressClick();
            (void)ActivateAsClick();
            if (m_repeat)
                StartRepeat();
        }
        void OnMouseUp(const MouseEvent& event) override
        {
            (void)event;
            m_repeating = false;
        }
        void OnPointerCancel(const MouseEvent&) override { m_repeating = false; }

        void OnKeyDown(const KeyEvent& event) override
        {
            if (event.Modifiers != 0 || !m_toggle)
                return;
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            // Down and the arrow toward the inline end move forward; Up and the arrow toward the
            // start move back.
            const i32 step = key == KeyCode::Down ? 1 : key == KeyCode::Up ? -1 : InlineStep(key);
            if (step == 0)
                return;
            core::Array<ButtonBase*> group;
            CollectGroup(group);
            if (group.Size() < 2)
                return;
            usize at = 0;
            while (at < group.Size() && group[at] != this)
                ++at;
            if (at == group.Size())
                return;
            // Find the next button round the group that can take the check.
            for (usize n = 1; n < group.Size(); ++n)
            {
                ButtonBase* next = group[step > 0 ? (at + n) % group.Size() : (at + group.Size() - n) % group.Size()];
                if (!next->IsTreeEnabled() || !next->IsDisplayed() || !next->IsTabFocusable())
                    continue;
                next->RequestFocus();
                if (!next->m_checked)
                    (void)next->ActivateAsClick();
                break;
            }
            event.Handled = true;
        }

        // The text part takes the button's font, colour and alignment, as a sheet's inheritance would.
        void OnTextStyleChange() override
        {
            if (!m_textPart)
                return;
            ShareTextStyle(*m_textPart);
            StyleWriteScope own(*m_textPart);
            m_textPart->SetTextAlignment(GetTextAlignH(), TextVAlign::Middle);
        }

    private:
        void ApplyText(core::StringView marked)
        {
            // The access key: `&File` shows `File`, with F as its key.
            const Mnemonic mnemonic = ParseMnemonic(marked);
            m_mnemonic = mnemonic.Key;
            const core::StringView text = mnemonic.Shown.AsView();
            if (text.Size() == 0 && !m_textPart)
                return;
            if (!m_textPart)
            {
                m_textPart = core::MakeRef<Label>(MemoryAllocator());
                m_textPart->SetTag(core::StringView(u8"text")); // tagged `text`, not `label`, so a sheet's label rules do not reach it
                m_textPart->SetHitTestVisible(false);
                {
                    StyleWriteScope defaults(*m_textPart);
                    m_textPart->SetTextAlignment(GetTextAlignH(), TextVAlign::Middle); // the button's alignment
                    // It takes the space the icon and the indicator leave, with its text aligned in it.
                    m_textPart->EditLayoutStyle([](LayoutStyle& s) { s.Grow = 1.0f; });
                }
                ShareTextStyle(*m_textPart);
                AddChildAt(m_textPart.Get(), PartIndex() + (m_iconPart ? 1 : 0));
            }
            m_textPart->SetMnemonicRange(mnemonic.Start, mnemonic.End);
            if (text == m_textPart->GetText())
                return;
            m_textPart->SetText(text);
            m_textPart->SetMnemonicRange(mnemonic.Start, mnemonic.End);
            m_textChanged.Notify();
        }
        void EnsureIconPart()
        {
            if (m_iconPart)
                return;
            m_iconPart = core::MakeRef<Image>(MemoryAllocator());
            m_iconPart->SetTag(core::StringView(u8"icon"));
            m_iconPart->SetHitTestVisible(false); // a press on it goes to the button
            m_iconPart->SetFit(ObjectFit::Contain);
            AddChildAt(m_iconPart.Get(), PartIndex());
        }
        // Returns where its first part goes: after its indicator.
        [[nodiscard]] usize PartIndex() const noexcept { return m_indicator ? 1 : 0; }

        [[nodiscard]] bool KeepsChecked() const
        {
            if (m_group)
                return !m_group->AllowsNone();
            return m_keepsChecked || !m_name.IsEmpty();
        }

        // Shows the icon for its state: the checked icon while checked (when it has one), else its
        // own.
        void ShowIconSource()
        {
            const core::String& path = m_checked && !m_checkedIconSource.IsEmpty() ? m_checkedIconSource : m_iconSource;
            if (path.IsEmpty())
            {
                SetIcon(core::RefPtr<Drawable>{});
                return;
            }
            EnsureIconPart();
            m_iconPart->SetSource(path);
        }

        void ApplyChecked(bool checked)
        {
            const bool settled = m_indeterminate;
            if (checked == m_checked && !settled)
                return;
            m_indeterminate = false; // a choice settles the mixed state
            const bool changed = checked != m_checked;
            m_checked = checked;
            MarkStyleStateDirty(StateChecked | StateIndeterminate);
            RedrawIndicator();
            if (changed)
            {
                if (!m_checkedIconSource.IsEmpty())
                    ShowIconSource(); // its checked icon, or its own again
                m_checkedChanged.Notify();
                OnCheckedChange();
            }
            // An exclusive group's others clear.
            if (m_checked)
            {
                core::Array<ButtonBase*> group;
                CollectGroup(group);
                for (ButtonBase* other : group)
                    if (other != this && other->m_toggle)
                        other->ApplyChecked(false);
            }
        }
        void ChangeCheckedByUser(bool checked)
        {
            const bool before = m_checked;
            ApplyChecked(checked);
            if (m_checked == before)
                return;
            if (m_onToggled)
                m_onToggled(m_checked);
            if (m_checked && m_group && m_group->m_onChanged)
                m_group->m_onChanged(this);
            SendEvent(Event(EventType::Changed, this));
        }
        void RedrawIndicator()
        {
            Invalidate();
            if (m_indicator)
                m_indicator->Invalidate();
        }

        void CollectNamed(const Node& node, core::Array<ButtonBase*>& out) const
        {
            for (usize i = 0; i < node.ChildCount(); ++i)
            {
                Node* child = node.GetChildAt(i);
                if (ButtonBase* b = core::Cast<ButtonBase>(child); b != nullptr && !b->m_group && b->m_name == m_name)
                    out.PushBack(b);
                CollectNamed(*child, out);
            }
        }

        // While held, it activates again after the delay, then at the interval, as long as the
        // pointer is over it.
        void StartRepeat()
        {
            m_repeating = true;
            m_repeatClock = -m_repeatDelay;
            if (m_repeatRunning || GetActionManager() == nullptr)
                return;
            m_repeatRunning = true;
            ButtonBase* self = this;
            RunAction(core::MakeRef<TickAction>(MemoryAllocator(), [self](core::Duration elapsed)
            {
                if (!self->m_repeating || !self->IsTreeEnabled())
                {
                    self->m_repeating = false;
                    self->m_repeatRunning = false;
                    return false;
                }
                if (!self->IsHovered())
                    return true; // the repeat pauses while the pointer is off it
                self->m_repeatClock += static_cast<f32>(elapsed.AsSecondsF());
                while (self->m_repeating && self->m_repeatClock >= self->m_repeatInterval)
                {
                    self->m_repeatClock -= self->m_repeatInterval;
                    (void)self->ActivateAsClick();
                }
                return true;
            }));
        }

        core::Function<void()> m_onClick;
        core::Function<void(bool)> m_onToggled;
        core::RefPtr<ButtonGroup> m_group;
        core::String m_name;
        core::String m_iconSource;        // `icon`
        core::String m_checkedIconSource; // `checked-icon`: shown while checked
        core::RefPtr<Label> m_textPart;
        char32_t m_mnemonic = 0; // its access key, from an `&` in its text
        core::RefPtr<Image> m_iconPart;
        core::RefPtr<ButtonIndicator> m_indicator;
        Observable m_checkedChanged; // bindings that read the state follow this
        Observable m_textChanged;
        ButtonAction m_actionOn = ButtonAction::Release;
        bool m_toggle = false;
        bool m_checked = false;
        bool m_indeterminate = false;
        bool m_keepsChecked = false;
        bool m_textFromCommand = false; // its text is its command's label (OnBoundCommandText)
        bool m_repeat = false;
        bool m_repeating = false;     // held and repeating
        bool m_repeatRunning = false; // its tick is running
        f32 m_repeatDelay = 0.5f;
        f32 m_repeatInterval = 0.08f;
        f32 m_repeatClock = 0.0f; // seconds toward the next repeat (below 0 during the delay)
    };

    inline ButtonIndicator::ButtonIndicator(ButtonBase& owner) : m_owner(owner)
    {
        SetTag(core::StringView(u8"indicator"));
        SetHitTestVisible(false); // a press on it goes to the button
    }
    inline core::Float2 ButtonIndicator::MeasureContent(core::Float2) { return m_owner.IndicatorSize(); }
    inline void ButtonIndicator::OnDraw(DrawContext& ctx, const Rect& localBounds)
    {
        (void)localBounds;
        m_owner.DrawIndicator(ctx, GetContentBounds());
    }

    inline ButtonBase* ButtonGroup::GetChecked() const
    {
        for (ButtonBase* b : m_buttons)
            if (b->IsChecked())
                return b;
        return nullptr;
    }
}
