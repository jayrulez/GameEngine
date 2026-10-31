// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :expander partition
//
// Expander: a header that shows or hides its content, like HTML's <details>. The header is a
// button (built on ButtonBase) whose indicator is a disclosure chevron and whose text is the
// expander's `title`. Activating the header toggles `open`, and each change sends a `toggle`
// event (a ToggleEvent). Expanders that share a `name` in one GUI are exclusive, as in HTML:
// opening one closes the others. A factory can build the content the first time it opens, and
// with `animate` the expander opens and closes by easing its height through a clip. Children
// written in markup become its content.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:expander;

import foundation.core; // Function, Move, String, RefPtr, Array, Duration, Min, Max
import :rect;
import :event;
import :draw_context;
import :node_box;
import :layout_style;
import :ui_widget;
import :panel;
import :reactive; // Observable, which bindings follow
import :button_base;
import :text; // TextHAlign
import :disclosure;
import :actions; // TickAction (the height animation)

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// An expander's header: a button whose indicator is the disclosure chevron, coloured by the
    /// `expanderheader::chevron` part and pointing down while the expander is open.
    class ExpanderHeader : public ButtonBase
    {
        RTTI_OBJECT(ExpanderHeader, ButtonBase)
    public:
        ExpanderHeader()
        {
            SetTag(core::StringView(u8"expanderheader"));
            {
                StyleWriteScope defaults(*this);
                SetTextAlignment(TextHAlign::Start, TextVAlign::Middle); // the text sits beside the indicator
            }
            MakeIndicator();
        }
        void SetOpenShown(bool open)
        {
            m_open = open;
            if (Node* indicator = GetIndicatorPart())
                indicator->Invalidate();
        }
        void CollectStyleParts(core::Array<core::StringView>& out) const override { out.PushBack(core::StringView(u8"chevron")); }
        // A drag that rests on a closed header opens the expander.
        bool OnDragHoverOpen(core::Float2) override
        {
            if (m_open)
                return false;
            (void)ActivateAsClick();
            return true;
        }

    protected:
        [[nodiscard]] core::Float2 IndicatorSize() const override { return core::Float2{12.0f, 12.0f}; }
        void DrawIndicator(DrawContext& ctx, const Rect& box) const override
        {
            const PartStyle& chevron = GetPartStyle(u8"chevron");
            Color color = chevron.HasBackground ? chevron.Box.BackgroundColor : Color{0.80f, 0.84f, 0.90f, 1.0f};
            color.a *= chevron.Opacity;
            DrawDisclosure(ctx, box, m_open, color, IsRightToLeft());
        }

    private:
        bool m_open = false;
    };

    class Expander : public UIWidget
    {
        RTTI_OBJECT(Expander, UIWidget)
    public:
        Expander()
        {
            SetTag(core::StringView(u8"expander"));
            Expander* self = this;
            m_header = core::MakeRef<ExpanderHeader>(MemoryAllocator());
            m_header->SetOnClick([self]() { self->SetOpen(!self->m_open); });
            AddChild(m_header.Get());
            // The content sits in a clip whose height the animation changes; the content keeps its
            // own height.
            m_clip = core::MakeRef<Panel>(MemoryAllocator());
            m_clip->SetClipChildren(true);
            m_clip->SetDisplayed(false);
            AddChild(m_clip.Get());
            m_content = core::MakeRef<Panel>(MemoryAllocator());
            m_content->SetTag(core::StringView(u8"expandercontent")); // styled by a sheet as `expander > * > expandercontent`
            m_content->EditLayoutStyle([](LayoutStyle& s) { s.Shrink = 0.0f; });
            m_clip->AddChild(m_content.Get());
        }

        /// Sets the header's text.
        void SetTitle(core::StringView title) { m_header->SetText(title); }
        [[nodiscard]] core::StringView GetTitle() const { return m_header->GetText(); }
        void SetTitleString(const core::String& title) { SetTitle(title.AsView()); }
        [[nodiscard]] core::String GetTitleString() const { return core::String(GetTitle()); }

        /// Opens or closes the expander, showing or hiding its content. Every change, whether the
        /// user's or the code's, sends a `toggle` event, as <details> does.
        void SetOpen(bool open);
        [[nodiscard]] bool IsOpen() const
        {
            m_openChanged.Track(); // a binding that reads this follows later changes
            return m_open;
        }
        void Toggle() { SetOpen(!m_open); }

        /// Sets the group name. Of the expanders that share a name in one GUI, only one is open at
        /// a time.
        void SetName(const core::String& name) { m_name = name; }
        [[nodiscard]] core::String GetName() const { return m_name; }

        /// Makes it open and close by easing its height rather than at once.
        void SetAnimated(bool animated) noexcept { m_animated = animated; }
        [[nodiscard]] bool IsAnimated() const noexcept { return m_animated; }

        /// Sets a factory that builds the content the first time the expander opens, so a heavy
        /// panel is not built until it is needed.
        void SetContentFactory(core::Function<void(Node& content)> factory) { m_factory = core::Move(factory); }
        /// Sets the callback for each change of `open`, by the user or by code.
        void SetOnToggled(core::Function<void(bool)> callback) { m_onToggled = core::Move(callback); }

        [[nodiscard]] ExpanderHeader* GetHeader() const noexcept { return m_header.Get(); }
        /// Returns the node that holds the content. Children written in markup go here.
        [[nodiscard]] Node* GetContent() const noexcept { return m_content.Get(); }
        [[nodiscard]] Node* GetChildHost() override { return m_content.Get(); }
        /// Returns how far open the expander is, from 0 to 1. Without animation this is 0 or 1.
        [[nodiscard]] f32 GetReveal() const noexcept { return m_reveal; }
        [[nodiscard]] u32 GetWidgetStates() const override { return m_open ? WidgetStateChecked : WidgetStateNone; } // :checked when open

    private:
        void CloseNamed(const Node& node)
        {
            for (usize i = 0; i < node.ChildCount(); ++i)
            {
                Node* child = node.GetChildAt(i);
                if (Expander* other = core::Cast<Expander>(child); other != nullptr && other != this && other->m_open && other->m_name == m_name)
                    other->SetOpen(false);
                CloseNamed(*child);
            }
        }
        // Eases the clip's height from its current value toward the content's height when
        // opening, or toward zero when closing. Once fully open, the clip drops its fixed height
        // so it follows the content again.
        void Animate()
        {
            m_clip->SetDisplayed(true);
            if (m_animating)
                return;
            m_animating = true;
            Expander* self = this;
            RunAction(core::MakeRef<TickAction>(MemoryAllocator(), [self](core::Duration elapsed)
            {
                constexpr f32 kSeconds = 0.18f;
                const f32 step = static_cast<f32>(elapsed.AsSecondsF()) / kSeconds;
                self->m_reveal = self->m_open ? core::Min(1.0f, self->m_reveal + step) : core::Max(0.0f, self->m_reveal - step);
                const f32 eased = self->m_reveal * self->m_reveal * (3.0f - 2.0f * self->m_reveal); // smoothstep
                const f32 full = self->m_content->GetSize().y;
                const bool done = self->m_open ? self->m_reveal >= 1.0f : self->m_reveal <= 0.0f;
                self->m_clip->EditLayoutStyle([&](LayoutStyle& s) { s.Height = done && self->m_open ? LayoutLength{} : LayoutLength::Px(full * eased); });
                if (!done)
                    return true;
                if (!self->m_open)
                    self->m_clip->SetDisplayed(false);
                self->m_animating = false;
                return false;
            }));
        }

        core::RefPtr<ExpanderHeader> m_header;
        core::RefPtr<Panel> m_clip;
        core::RefPtr<Panel> m_content;
        core::String m_name;
        bool m_open = false;
        Observable m_openChanged; // notifies bindings that read `open`
        bool m_animated = false;
        bool m_animating = false;
        f32 m_reveal = 0.0f;
        core::Function<void(Node&)> m_factory;
        core::Function<void(bool)> m_onToggled;
    };

    inline void Expander::SetOpen(bool open)
    {
        if (open == m_open)
            return;
        m_open = open;
        m_openChanged.Notify();
        m_header->SetOpenShown(open);
        MarkStyleStateDirty(StateChecked); // `expander:checked` styles an open expander
        if (open)
        {
            if (m_factory)
            {
                core::Function<void(Node&)> factory = core::Move(m_factory);
                factory(*m_content);
            }
            // Opening one expander of a named group closes the others.
            if (!m_name.IsEmpty())
            {
                const Node* top = this;
                while (top->GetParent() != nullptr)
                    top = top->GetParent();
                CloseNamed(*top);
            }
        }
        if (m_animated && GetActionManager() != nullptr)
            Animate();
        else
        {
            m_reveal = open ? 1.0f : 0.0f;
            m_clip->EditLayoutStyle([](LayoutStyle& s) { s.Height = LayoutLength{}; });
            m_clip->SetDisplayed(open);
        }
        if (m_onToggled)
            m_onToggled(open);
        SendEvent(ToggleEvent(this, open, PopupCloseReason::Code));
    }
}
