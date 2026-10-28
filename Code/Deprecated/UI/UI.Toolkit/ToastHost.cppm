// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :toast_host partition
//
// Transient notification overlay ("toasts"): a full-viewport pass-through layer that stacks
// notification cards in its bottom-right corner, newest nearest the corner. Not a Sedulous port
// (upstream has no toast control) - designed for the editor but app-agnostic.
//
// Behavior contract:
//  - Show() adds a card; durationSeconds > 0 auto-expires it, <= 0 is STICKY (stays until the
//    user closes it or the app calls Dismiss). Errors and action-bearing toasts should be sticky.
//  - The host itself is input-transparent (IsHitTestVisible = false leaves only the cards
//    hittable), so it can overlay the whole window root.
//  - Card buttons only MARK a toast closing; the view is removed on the next Update() - never
//    mid-event-dispatch (the UI mutation-queue rule, without needing the queue).
//  - The owning app calls Update(dt) once per frame (same idiom as the editor inspector's
//    Refresh) - there is no self-ticking.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui.toolkit:toast_host;

import foundation.core;
import foundation.vg;
import foundation.fonts;
import foundation.ui;

using namespace foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::ui::toolkit
{
    enum class ToastSeverity : u8
    {
        Info,
        Success,
        Warning,
        Error
    };

    /// One notification. `durationSeconds <= 0` = sticky (until closed/dismissed).
    struct ToastRequest
    {
        String message;
        ToastSeverity severity = ToastSeverity::Info;
        f32 durationSeconds = 5.0f;
        String actionLabel;        // empty = no action button
        Function<void()> onAction; // fired on action click (the toast then closes)
    };

    /// A single toast card: severity accent bar + message + optional action + close button.
    class ToastCard : public FlexLayout
    {
        RTTI_OBJECT(ToastCard, FlexLayout)
    public:
        Color Accent{0.35f, 0.55f, 0.95f, 1.0f};

        ToastCard()
        {
            Direction = Orientation::Horizontal;
            Spacing = 8.0f;
            Padding = Thickness(10.0f);
        }

        void OnDraw(UIDrawContext& ctx) override
        {
            const Color bg =
                ResolveStyleColor(StyleProperty::Background, Color{0.13f, 0.14f, 0.17f, 0.97f});
            const f32 r = ResolveStyleFloat(StyleProperty::CornerRadius, 0.0f);
            const Rectangle bounds{0, 0, Width(), Height()};
            if (r > 0.0f)
            {
                ctx.VG().FillRoundedRect(bounds, r, bg);
                // Accent trim drawn OVER the card's left edge. Its width equals the corner radius so its
                // rounded left corners coincide exactly with the card's - a thinner bar can't match a
                // larger radius and reads as a separate rounded box floating inside the card.
                ctx.VG().FillRoundedRect(Rectangle{0, 0, r, Height()},
                                         foundation::vg::CornerRadii{r, 0.0f, 0.0f, r}, Accent);
            }
            else
            {
                ctx.VG().FillRect(bounds, bg);
                ctx.VG().FillRect(Rectangle{0, 0, 3.0f, Height()}, Accent);
            }
            DrawChildren(ctx);
        }
    };

    /// Bottom-right toast stack. Attach to a window's RootView (it fills the viewport and
    /// passes input through outside the cards); call Update(dt) once per frame.
    class ToastHost : public ViewGroup
    {
        RTTI_OBJECT(ToastHost, ViewGroup)
    public:
        f32 ToastWidth = 340.0f;
        f32 CornerMargin = 12.0f;
        f32 Spacing = 8.0f;

        ToastHost() { IsHitTestVisible = false; } // only the cards take input

        /// Adds a toast; returns its id (for Dismiss / Contains).
        u64 Show(ToastRequest request)
        {
            const u64 id = m_nextId++;

            RefPtr<ToastCard> card = MakeRef<ToastCard>(MemoryAllocator());
            // Every severity resolves through the theme (the semantic status properties map to
            // the palette's $success/$warning/$error); AccentFor stays as the unthemed fallback.
            card->Accent = ResolveStyleColor(SeverityProperty(request.severity),
                                             AccentFor(request.severity));

            RefPtr<Label> message = MakeRef<Label>(MemoryAllocator());
            message->SetText(request.message.AsView());
            message->FontSize.SetValue(12.0f);
            message->VAlign.SetValue(fonts::VerticalAlignment::Middle);
            {
                LayoutStyle lp;
                lp.FlexGrow = 1.0f;
                lp.Height = SizeSpec::Match();
                card->AddView(message.Get(), lp);
            }

            ToastHost* self = this;
            if (!request.actionLabel.IsEmpty())
            {
                RefPtr<Button> action =
                    MakeRef<Button>(MemoryAllocator(), request.actionLabel.AsView());
                action->OnClick.Add(
                    [self, id](ButtonBase*)
                    {
                        self->InvokeAction(id);
                        self->MarkClosing(id);
                    });
                card->AddView(action.Get());
            }

            RefPtr<Button> close = MakeRef<Button>(MemoryAllocator(), StringView(u8"×"));
            close->OnClick.Add([self, id](ButtonBase*) { self->MarkClosing(id); });
            card->AddView(close.Get());

            AddView(card.Get());

            Entry entry;
            entry.id = id;
            entry.duration = request.durationSeconds;
            entry.card = card.Get();
            entry.onAction = Move(request.onAction);
            m_entries.PushBack(Move(entry));
            Invalidate();
            return id;
        }

        /// Marks a toast for removal (the card leaves on the next Update).
        void Dismiss(u64 id) { MarkClosing(id); }

        /// Live (not yet removed) toasts.
        [[nodiscard]] usize ToastCount() const noexcept { return m_entries.Size(); }

        [[nodiscard]] bool Contains(u64 id) const noexcept
        {
            for (const Entry& e : m_entries)
            {
                if (e.id == id)
                {
                    return true;
                }
            }
            return false;
        }

        /// Per-frame: ages timed toasts, removes expired/closed cards (outside event dispatch).
        void Update(f32 dt)
        {
            bool changed = false;
            for (Entry& e : m_entries)
            {
                if (!e.closing && e.duration > 0.0f)
                {
                    e.age += dt;
                    if (e.age >= e.duration)
                    {
                        e.closing = true;
                    }
                }
            }
            for (usize i = m_entries.Size(); i-- > 0;)
            {
                if (m_entries[i].closing)
                {
                    RemoveView(m_entries[i].card, true);
                    m_entries.RemoveAt(i);
                    changed = true;
                }
            }
            if (changed)
            {
                Invalidate();
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            const BoxConstraints cardConstraints{ToastWidth, ToastWidth, 0.0f, 200.0f};
            for (const Entry& e : m_entries)
            {
                e.card->Measure(cardConstraints);
            }
            MeasuredSize = Float2{constraints.MaxWidth, constraints.MaxHeight};
        }

        void OnLayout(f32, f32, f32 width, f32 height) override
        {
            // Newest nearest the bottom-right corner, stacking upward.
            f32 y = height - CornerMargin;
            for (usize i = m_entries.Size(); i-- > 0;)
            {
                View* card = m_entries[i].card;
                const f32 h = card->MeasuredSize.y;
                y -= h;
                card->Layout(width - CornerMargin - ToastWidth, y, ToastWidth, h);
                y -= Spacing;
            }
        }

    private:
        struct Entry
        {
            u64 id = 0;
            f32 age = 0.0f;
            f32 duration = 0.0f;
            bool closing = false;
            View* card = nullptr; // borrowed; the child tree owns it
            Function<void()> onAction;
        };

        void MarkClosing(u64 id)
        {
            for (Entry& e : m_entries)
            {
                if (e.id == id)
                {
                    e.closing = true;
                    return;
                }
            }
        }

        void InvokeAction(u64 id)
        {
            for (Entry& e : m_entries)
            {
                if (e.id == id)
                {
                    if (e.onAction)
                    {
                        e.onAction();
                    }
                    return;
                }
            }
        }

        [[nodiscard]] static StyleProperty SeverityProperty(ToastSeverity severity)
        {
            switch (severity)
            {
            case ToastSeverity::Success:
                return StyleProperty::SuccessColor;
            case ToastSeverity::Warning:
                return StyleProperty::WarningColor;
            case ToastSeverity::Error:
                return StyleProperty::ErrorColor;
            case ToastSeverity::Info:
            default:
                return StyleProperty::AccentColor;
            }
        }

        [[nodiscard]] static Color AccentFor(ToastSeverity severity)
        {
            switch (severity)
            {
            case ToastSeverity::Success:
                return Color{0.30f, 0.75f, 0.40f, 1.0f};
            case ToastSeverity::Warning:
                return Color{0.95f, 0.70f, 0.25f, 1.0f};
            case ToastSeverity::Error:
                return Color{0.90f, 0.30f, 0.30f, 1.0f};
            case ToastSeverity::Info:
            default:
                return Color{0.35f, 0.55f, 0.95f, 1.0f};
            }
        }

        Array<Entry> m_entries;
        u64 m_nextId = 1;
    };

    RTTI_DEFINE_OBJECT(ToastCard, "rtti::ui::toolkit")
    RTTI_DEFINE_OBJECT(ToastHost, "rtti::ui::toolkit")
}
