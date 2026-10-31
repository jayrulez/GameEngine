// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :toast partition
//
// Toasts: short notices that go away by themselves, with one host serving games and the editor
// alike. A ToastHost is a widget the application places over a window. Only its cards take the
// pointer, and a toast never takes focus.
//
// A request carries the content: a message (and a title), a widget shown inside the host's card,
// or, with `Bare`, the application's own widget with no card. It also carries a severity (info,
// success, warning or error, whose colour and icon come from the style sheet), up to two actions,
// a duration and a key. By default info and success toasts stay 5 s, a warning 8 s, and an error
// until closed; the host can change each default. Hovering or focusing any toast pauses them all.
// A post whose key matches a shown toast replaces it and increments its count ("×12"), as for a
// burst of the same error or an updated progress toast. Four toasts show at once and the rest wait.
//
// Posts may come from any thread. They wait in the host until its next frame, and a content
// widget is then made there by a function, since widgets do not cross threads. A card enters and
// leaves through the widget's Enter and Leave (`:entering`, `:leaving`), so the sheet's
// transitions play and a dismissed card is removed once its exit has ended. A request's classes
// choose the transition (the theme's slide, `pop`, `still`, or a game's own); reduced motion only
// fades. Each shown toast is announced to screen readers, and an error interrupts.
//
// The host keeps no history; instead it tells the application about each toast posted and gone,
// for a log or a notification bell. A request is a plain value with no callbacks in it, so a
// script fills one in the same way code does and posts it. An action's button sends `activate`
// with the action's id, as a menu item does, in addition to any callback passed with the post.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:toast;

import foundation.core; // RefPtr, MakeRef, Array, String, Function, Mutex, ScopedLock, Move, Max
import :rect;
import :event;
import :node;
import :ui_widget;
import :layout_style;
import :label;
import :button;
import :model_index; // ModelIndex, for an action's `activate`
import :actions;       // TickAction, which runs on the GUI's clock
import :accessibility; // AnnouncePriority
import :scene_root;    // GUIContext::Announce

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    enum class ToastSeverity : u8
    {
        Info,
        Success,
        Warning,
        Error,
    };
    [[nodiscard]] inline core::StringView ToastSeverityName(ToastSeverity severity) noexcept
    {
        switch (severity)
        {
        case ToastSeverity::Success: return core::StringView(u8"success");
        case ToastSeverity::Warning: return core::StringView(u8"warning");
        case ToastSeverity::Error: return core::StringView(u8"error");
        case ToastSeverity::Info:
        default: return core::StringView(u8"info");
        }
    }

    /// A button on a toast. Pressing it closes the toast, runs its command if it names one, calls
    /// the post's callback with the action, and sends `activate` with its id from the button, for
    /// scripts.
    struct ToastAction
    {
        core::String Id;
        core::String Text;
        core::String Command;
    };

    using ToastId = u64;

    /// What a toast shows, and for how long.
    struct ToastRequest
    {
        core::String Title;
        core::String Message;
        bool Rich = false; ///< the message is Label rich text (markup)
        /// Content shown in the host's card instead of the title and message. Only usable on the
        /// GUI's thread; a post from another thread passes a function that makes it instead.
        core::RefPtr<UIWidget> Content;
        /// Shows the content alone with no card, while the host still controls its lifetime and
        /// place (a game's banner, say).
        bool Bare = false;
        /// What a screen reader announces. When empty, the title and message are announced, or else
        /// the content's accessible name.
        core::String Announcement;
        ToastSeverity Severity = ToastSeverity::Info;
        core::Array<ToastAction> Actions; ///< at most two are shown; the rest are ignored
        /// How many seconds the toast stays. Below 0 means the severity's default; 0 means until
        /// closed.
        f32 Duration = -1.0f;
        /// A post with the same key as a shown or waiting toast replaces it and increments its
        /// count. An empty key never matches.
        core::String Key;
        /// Classes for the card, separated by spaces. They select how it enters and leaves, as the
        /// sheet defines (the theme's `pop` and `still`, or a game's own), or any other look the
        /// sheet gives them.
        core::String Classes;

        /// Adds an action. Scripts use this because an element added to a list there is a copy. An
        /// empty command means the action runs no command.
        void AddAction(core::String id, core::String text, core::String command)
        {
            Actions.PushBack(ToastAction{core::Move(id), core::Move(text), core::Move(command)});
        }
    };
    /// Called with the action pressed on a toast.
    using ToastActionCallback = core::Function<void(const ToastAction&)>;
    /// Makes a toast's content; called on the GUI's thread.
    using ToastContentMaker = core::Function<core::RefPtr<UIWidget>()>;

    class ToastHost;

    /// One toast (`toast`, with its severity's class): a card holding its parts, or bare content.
    class ToastCard : public UIWidget
    {
        RTTI_OBJECT(ToastCard, UIWidget)
    public:
        ToastCard()
        {
            SetTag(core::StringView(u8"toast"));
            StyleWriteScope defaults(*this);
            EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Row;
                s.AlignItems = Align::Start;
                s.ColumnGap = 8.0f;
                s.Shrink = 0.0f;
            });
        }
        [[nodiscard]] ToastId GetToastId() const noexcept { return m_id; }
        [[nodiscard]] ToastSeverity GetSeverity() const noexcept { return m_severity; }
        [[nodiscard]] core::StringView GetKey() const noexcept { return m_key.AsView(); }
        /// Returns how many posts the toast stands for: 1, plus one for each post that replaced it
        /// by key.
        [[nodiscard]] u32 GetCount() const noexcept { return m_count; }
        /// Returns the seconds left. With a duration, 0 or less means it is going; without one
        /// (until closed) the value stays below 0.
        [[nodiscard]] f32 GetRemaining() const noexcept { return m_remaining; }
        [[nodiscard]] Label* GetTitlePart() const noexcept { return m_title.Get(); }
        [[nodiscard]] Label* GetMessagePart() const noexcept { return m_message.Get(); }
        [[nodiscard]] Label* GetCountPart() const noexcept { return m_countPart.Get(); }
        [[nodiscard]] UIWidget* GetContent() const noexcept { return m_content.Get(); }
        [[nodiscard]] Button* GetCloseButton() const noexcept { return m_close.Get(); }
        [[nodiscard]] usize ActionCount() const noexcept { return m_actions.Size(); }
        [[nodiscard]] Button& ActionAt(usize index) const noexcept { return *m_actions[index]; }
        /// Returns what the toast announces to a screen reader.
        [[nodiscard]] core::StringView GetAnnouncement() const noexcept { return m_announcement.AsView(); }

    private:
        friend class ToastHost;

        ToastId m_id = 0;
        ToastSeverity m_severity = ToastSeverity::Info;
        core::String m_key;
        core::String m_announcement;
        u32 m_count = 1;
        f32 m_remaining = 0.0f;
        bool m_dismissed = false; // taken off the stack and leaving (Leave); removed once its exit ends
        core::String m_classes; // the classes from the request
        core::RefPtr<UIWidget> m_body;
        core::RefPtr<Label> m_title;
        core::RefPtr<Label> m_message;
        core::RefPtr<Label> m_countPart;
        core::RefPtr<UIWidget> m_content;
        core::RefPtr<UIWidget> m_actionRow;
        core::Array<core::RefPtr<Button>> m_actions;
        core::Array<ToastAction> m_actionItems;
        ToastActionCallback m_onAction;
        core::RefPtr<Button> m_close;
    };

    class ToastHost : public UIWidget
    {
        RTTI_OBJECT(ToastHost, UIWidget)
    public:
        static constexpr usize kMaxActions = 2;

        ToastHost()
        {
            SetTag(core::StringView(u8"toasthost"));
            SetHitTestVisible(false); // only the cards take the pointer
            {
                StyleWriteScope defaults(*this);
                EditLayoutStyle([](LayoutStyle& s)
                {
                    // Cover the parent, with the toasts stacked at the bottom end.
                    s.Placement = Position::Absolute;
                    s.Left = LayoutLength::Px(0.0f);
                    s.Top = LayoutLength::Px(0.0f);
                    s.Right = LayoutLength::Px(0.0f);
                    s.Bottom = LayoutLength::Px(0.0f);
                    s.Direction = FlexDirection::Column;
                    s.JustifyContent = Justify::End;
                    s.AlignItems = Align::End;
                    s.RowGap = 8.0f;
                    s.ZIndex = 1000;
                });
                SetPadding(Thickness{16.0f, 16.0f, 16.0f, 16.0f});
            }
            m_defaults[static_cast<usize>(ToastSeverity::Info)] = 5.0f;
            m_defaults[static_cast<usize>(ToastSeverity::Success)] = 5.0f;
            m_defaults[static_cast<usize>(ToastSeverity::Warning)] = 8.0f;
            m_defaults[static_cast<usize>(ToastSeverity::Error)] = 0.0f;
        }

        /// Shows a toast, or queues it while the maximum number are already shown. Call this on
        /// the GUI's thread. `onAction` is called with the action pressed.
        ToastId Post(ToastRequest request, ToastActionCallback onAction = {})
        {
            ToastId id = 0;
            {
                core::ScopedLock<core::Mutex> lock(m_lock);
                id = ++m_lastId;
            }
            KeepTicking();
            Place(id, core::Move(request), core::Move(onAction));
            return id;
        }
        /// Posts from any thread; the toast waits until the host's next frame. Any content comes from
        /// `makeContent`, which is called then on the GUI's thread; the request's `Content` is ignored.
        ToastId PostFromAnyThread(ToastRequest request, ToastContentMaker makeContent = {}, ToastActionCallback onAction = {})
        {
            request.Content = core::RefPtr<UIWidget>{};
            core::ScopedLock<core::Mutex> lock(m_lock);
            const ToastId id = ++m_lastId;
            m_incoming.PushBack(Incoming{id, core::Move(request), core::Move(makeContent), core::Move(onAction)});
            return id;
        }
        /// Removes a toast, shown or waiting. Returns false when there is no such toast.
        bool Dismiss(ToastId id) { return Remove(id, false); }
        /// Removes every toast.
        void DismissAll()
        {
            while (!m_waiting.IsEmpty())
                (void)Remove(m_waiting[0].Id, false);
            for (usize i = m_cards.Size(); i-- > 0;)
                if (!m_cards[i]->m_dismissed)
                    (void)Remove(m_cards[i]->m_id, false);
        }

        // Reflected for scripts as `host.post(request)` and `host.postContent(request, widget)`.
        ToastId PostScript(ToastRequest request) { return Post(core::Move(request)); }
        ToastId PostContentScript(ToastRequest request, core::Variant content)
        {
            Node* node = Node::FromHandle(content);
            request.Content = core::RefPtr<UIWidget>(node != nullptr ? Cast<UIWidget>(node) : nullptr);
            return Post(core::Move(request));
        }
        [[nodiscard]] i32 GetShownCountScript() const noexcept { return static_cast<i32>(ShownCount()); }
        [[nodiscard]] i32 GetWaitingCountScript() const noexcept { return static_cast<i32>(WaitingCount()); }
        [[nodiscard]] i32 GetMaxShownScript() const noexcept { return static_cast<i32>(m_maxShown); }
        void SetMaxShownScript(i32 count) noexcept { SetMaxShown(static_cast<usize>(core::Max(1, count))); }

        /// Returns how many toasts are shown now, not counting those leaving. WaitingCount gives the
        /// number queued.
        [[nodiscard]] usize ShownCount() const noexcept
        {
            usize count = 0;
            for (const core::RefPtr<ToastCard>& card : m_cards)
                count += !card->m_dismissed ? 1 : 0;
            return count;
        }
        [[nodiscard]] usize WaitingCount() const noexcept { return m_waiting.Size(); }
        /// Returns a shown toast's card, or null when it is not shown.
        [[nodiscard]] ToastCard* GetCard(ToastId id) const noexcept
        {
            for (const core::RefPtr<ToastCard>& card : m_cards)
                if (card->m_id == id && !card->m_dismissed)
                    return card.Get();
            return nullptr;
        }
        [[nodiscard]] bool IsShown(ToastId id) const noexcept { return GetCard(id) != nullptr; }
        [[nodiscard]] bool IsWaiting(ToastId id) const noexcept
        {
            for (const Waiting& waiting : m_waiting)
                if (waiting.Id == id)
                    return true;
            return false;
        }
        /// Returns whether the timers are paused, which they are while the pointer or the focus is
        /// on a toast.
        [[nodiscard]] bool IsPaused() const
        {
            for (const core::RefPtr<ToastCard>& card : m_cards)
                if (card->IsHovered() || card->IsFocusWithin())
                    return true;
            return false;
        }

        /// Sets how many toasts show at once (4 by default); the rest wait.
        void SetMaxShown(usize count) noexcept { m_maxShown = core::Max<usize>(1, count); }
        [[nodiscard]] usize GetMaxShown() const noexcept { return m_maxShown; }
        /// Sets how many seconds a severity's toasts stay when a request does not say; 0 means
        /// until closed.
        void SetDefaultDuration(ToastSeverity severity, f32 seconds) noexcept { m_defaults[static_cast<usize>(severity)] = core::Max(0.0f, seconds); }
        [[nodiscard]] f32 GetDefaultDuration(ToastSeverity severity) const noexcept { return m_defaults[static_cast<usize>(severity)]; }

        /// Sets the callback told about each toast posted, for a log. It runs when a toast is shown
        /// or replaces a shown one.
        void SetOnPosted(core::Function<void(ToastId, const ToastCard&)> callback) { m_onPosted = core::Move(callback); }
        /// Sets the callback told about each toast that goes, and whether the user closed it (with
        /// its close button or an action).
        void SetOnDismissed(core::Function<void(ToastId, bool byUser)> callback) { m_onDismissed = core::Move(callback); }

        /// Advances the toasts by `seconds`. The GUI's clock calls this once per host frame.
        void Tick(f32 seconds)
        {
            // Take in what other threads posted, now that this runs on the GUI's thread.
            core::Array<Incoming> incoming(MemoryAllocator());
            {
                core::ScopedLock<core::Mutex> lock(m_lock);
                incoming = core::Move(m_incoming);
                m_incoming = core::Array<Incoming>(MemoryAllocator());
            }
            for (Incoming& post : incoming)
            {
                if (post.MakeContent)
                    post.Request.Content = post.MakeContent();
                Place(post.Id, core::Move(post.Request), core::Move(post.OnAction));
            }
            const bool paused = IsPaused();
            for (usize i = m_cards.Size(); i-- > 0;)
            {
                ToastCard& card = *m_cards[i];
                if (card.m_dismissed)
                {
                    if (card.GetParent() != this) // its exit ended and Leave removed it
                        m_cards.RemoveAt(i);
                    continue;
                }
                if (paused || card.m_remaining < 0.0f)
                    continue;
                card.m_remaining -= seconds;
                if (card.m_remaining <= 0.0f)
                    (void)Remove(card.m_id, false);
            }
            ShowWaiting();
        }

    protected:
        // Layout happens once the host is in a GUI, so start its clock here; an action runs only
        // under a root.
        void OnLayout() override
        {
            UIWidget::OnLayout();
            KeepTicking();
        }

    private:
        void KeepTicking()
        {
            if (m_ticking || GetActionManager() == nullptr)
                return;
            m_ticking = true;
            ToastHost* self = this;
            RunAction(core::MakeRef<TickAction>(MemoryAllocator(), [self](core::Duration elapsed)
                                                {
                                                    self->Tick(static_cast<f32>(elapsed.AsSeconds()));
                                                    return true;
                                                }));
        }
        struct Incoming
        {
            ToastId Id = 0;
            ToastRequest Request;
            ToastContentMaker MakeContent;
            ToastActionCallback OnAction;
        };
        struct Waiting
        {
            ToastId Id = 0;
            ToastRequest Request;
            u32 Count = 1;
            ToastActionCallback OnAction;
        };

        [[nodiscard]] f32 DurationOf(const ToastRequest& request) const
        {
            const f32 seconds = request.Duration < 0.0f ? m_defaults[static_cast<usize>(request.Severity)] : request.Duration;
            return seconds > 0.0f ? seconds : -1.0f; // below 0 means until closed
        }
        // Shows the toast, replaces the one with the same key, or queues it.
        void Place(ToastId id, ToastRequest request, ToastActionCallback onAction)
        {
            if (!request.Key.IsEmpty())
            {
                for (const core::RefPtr<ToastCard>& card : m_cards)
                    if (!card->m_dismissed && card->m_key == request.Key)
                    {
                        const u32 count = card->m_count + 1;
                        Fill(*card, card->m_id, core::Move(request), count, core::Move(onAction));
                        if (m_onPosted)
                            m_onPosted(card->m_id, *card);
                        Announce(*card);
                        return;
                    }
                for (Waiting& waiting : m_waiting)
                    if (waiting.Request.Key == request.Key)
                    {
                        waiting.Request = core::Move(request);
                        waiting.OnAction = core::Move(onAction);
                        ++waiting.Count;
                        return;
                    }
            }
            if (ShownCount() >= m_maxShown)
            {
                m_waiting.PushBack(Waiting{id, core::Move(request), 1, core::Move(onAction)});
                return;
            }
            Show(id, core::Move(request), 1, core::Move(onAction));
        }
        void Show(ToastId id, ToastRequest request, u32 count, ToastActionCallback onAction)
        {
            core::RefPtr<ToastCard> card = core::MakeRef<ToastCard>(MemoryAllocator());
            Fill(*card, id, core::Move(request), count, core::Move(onAction));
            AddChild(card.Get());
            card->Enter(); // sets `:entering` so the sheet's entrance plays
            m_cards.PushBack(card);
            if (m_onPosted)
                m_onPosted(id, *card);
            Announce(*card);
        }
        void ShowWaiting()
        {
            while (!m_waiting.IsEmpty() && ShownCount() < m_maxShown)
            {
                Waiting next = core::Move(m_waiting[0]);
                m_waiting.RemoveAt(0);
                Show(next.Id, core::Move(next.Request), next.Count, core::Move(next.OnAction));
            }
        }
        // Builds the card's parts from the request, including for a replacement, which carries its
        // count.
        void Fill(ToastCard& card, ToastId id, ToastRequest request, u32 count, ToastActionCallback onAction)
        {
            card.m_id = id;
            card.m_count = count;
            card.m_key = request.Key;
            card.m_remaining = DurationOf(request);
            card.RemoveClass(ToastSeverityName(card.m_severity));
            card.m_severity = request.Severity;
            card.AddClass(ToastSeverityName(card.m_severity));
            EachClass(card.m_classes.AsView(), [&card](core::StringView name) { card.RemoveClass(name); });
            card.m_classes = request.Classes;
            EachClass(card.m_classes.AsView(), [&card](core::StringView name) { card.AddClass(name); });
            if (request.Bare)
                card.AddClass(core::StringView(u8"bare"));
            else
                card.RemoveClass(core::StringView(u8"bare"));
            while (card.ChildCount() != 0)
                card.RemoveChild(card.GetChildAt(card.ChildCount() - 1));
            card.m_title = core::RefPtr<Label>{};
            card.m_message = core::RefPtr<Label>{};
            card.m_countPart = core::RefPtr<Label>{};
            card.m_close = core::RefPtr<Button>{};
            card.m_actions.Clear();
            card.m_actionItems.Clear();
            card.m_onAction = core::Move(onAction);
            card.m_content = request.Content;
            if (request.Bare && card.m_content)
            {
                card.AddChild(card.m_content.Get());
                card.m_announcement = request.Announcement.IsEmpty() ? card.m_content->GetAccessLabel() : request.Announcement;
                return;
            }
            // The card holds its mark (the sheet's colour and icon), its body, its count and its
            // close button.
            auto mark = core::MakeRef<UIWidget>(MemoryAllocator());
            mark->SetTag(core::StringView(u8"toastmark"));
            mark->SetHitTestVisible(false);
            card.AddChild(mark.Get());
            card.m_body = core::MakeRef<UIWidget>(MemoryAllocator());
            card.m_body->AddClass(core::StringView(u8"body"));
            card.m_body->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Direction = FlexDirection::Column;
                s.RowGap = 4.0f;
                s.Grow = 1.0f;
                s.Shrink = 1.0f;
                s.MinWidth = LayoutLength::Px(0.0f);
            });
            card.AddChild(card.m_body.Get());
            core::String said;
            if (card.m_content)
                card.m_body->AddChild(card.m_content.Get());
            else
            {
                if (!request.Title.IsEmpty())
                {
                    card.m_title = core::MakeRef<Label>(MemoryAllocator());
                    card.m_title->AddClass(core::StringView(u8"title"));
                    card.m_title->SetText(request.Title.AsView());
                    card.m_body->AddChild(card.m_title.Get());
                    said = request.Title;
                }
                card.m_message = core::MakeRef<Label>(MemoryAllocator());
                card.m_message->AddClass(core::StringView(u8"message"));
                card.m_message->SetRich(request.Rich);
                card.m_message->SetText(request.Message.AsView());
                card.m_body->AddChild(card.m_message.Get());
                if (!said.IsEmpty() && !request.Message.IsEmpty())
                    said.Append(core::StringView(u8": "));
                said.Append(card.m_message->GetTextLayout().PlainText()); // a rich message is announced without its markup
            }
            card.m_announcement = !request.Announcement.IsEmpty() ? request.Announcement
                                  : card.m_content                ? card.m_content->GetAccessLabel()
                                                                  : said;
            ToastHost* self = this;
            const usize actions = core::Min(request.Actions.Size(), kMaxActions);
            if (actions > 0)
            {
                card.m_actionRow = core::MakeRef<UIWidget>(MemoryAllocator());
                card.m_actionRow->AddClass(core::StringView(u8"actions"));
                card.m_actionRow->EditLayoutStyle([](LayoutStyle& s)
                {
                    s.Direction = FlexDirection::Row;
                    s.ColumnGap = 6.0f;
                });
                card.m_body->AddChild(card.m_actionRow.Get());
                for (usize i = 0; i < actions; ++i)
                {
                    const ToastAction& action = request.Actions[i];
                    core::RefPtr<Button> button = core::MakeRef<Button>(MemoryAllocator());
                    button->AddClass(core::StringView(u8"action"));
                    button->SetText(action.Text.AsView());
                    button->SetId(action.Id.AsView()); // the id its `activate` event carries
                    card.m_actionItems.PushBack(action);
                    if (!action.Command.IsEmpty())
                        button->SetCommand(action.Command.AsView());
                    ToastCard* held = &card;
                    button->SetOnClick([self, held, i]()
                    {
                        const ToastId toast = held->m_id;
                        if (i >= held->m_actionItems.Size())
                            return;
                        const ToastAction pressed = held->m_actionItems[i];
                        core::RefPtr<Button> from = held->m_actions[i];
                        if (held->m_onAction)
                            held->m_onAction(pressed);
                        // Send `activate` for scripts, as a menu item does. It bubbles and carries
                        // the action's id.
                        ItemEvent activated(EventType::ItemActivate, from.Get(), static_cast<i32>(i), ModelIndex{});
                        (void)from->RouteEvent(activated);
                        (void)self->Remove(toast, true);
                    });
                    card.m_actionRow->AddChild(button.Get());
                    card.m_actions.PushBack(core::Move(button));
                }
            }
            if (count > 1)
            {
                card.m_countPart = core::MakeRef<Label>(MemoryAllocator());
                card.m_countPart->AddClass(core::StringView(u8"count"));
                card.m_countPart->SetText(core::Format(u8"×{}", count).AsView());
                card.AddChild(card.m_countPart.Get());
            }
            card.m_close = core::MakeRef<Button>(MemoryAllocator());
            card.m_close->AddClass(core::StringView(u8"close"));
            card.m_close->SetText(core::StringView(u8"×"));
            card.m_close->SetAccessLabel(core::String(core::StringView(u8"Close")));
            ToastCard* held = &card;
            card.m_close->SetOnClick([self, held]() { (void)self->Remove(held->m_id, true); });
            card.AddChild(card.m_close.Get());
        }
        template <typename Fn>
        static void EachClass(core::StringView classes, Fn&& each)
        {
            core::StringView rest = core::Trim(classes);
            while (rest.Size() != 0)
            {
                usize end = 0;
                while (end < rest.Size() && rest[end] != u8' ')
                    ++end;
                each(rest.SubStr(0, end));
                rest = core::Trim(rest.SubStr(end, rest.Size() - end));
            }
        }
        void Announce(const ToastCard& card)
        {
            if (card.m_announcement.IsEmpty())
                return;
            if (SceneRoot* scene = GetSceneRoot())
                scene->GetContext().Announce(card.m_announcement.AsView(),
                                             card.m_severity == ToastSeverity::Error ? AnnouncePriority::Assertive : AnnouncePriority::Polite);
        }
        // Takes a toast off the stack. A waiting toast goes at once. A shown toast leaves through
        // Leave: it stops taking the pointer at once, stays a child until its exit has ended, and is
        // never removed inside the event that closed it.
        bool Remove(ToastId id, bool byUser)
        {
            for (usize i = 0; i < m_waiting.Size(); ++i)
                if (m_waiting[i].Id == id)
                {
                    m_waiting.RemoveAt(i);
                    if (m_onDismissed)
                        m_onDismissed(id, byUser);
                    return true;
                }
            for (const core::RefPtr<ToastCard>& card : m_cards)
                if (card->m_id == id && !card->m_dismissed)
                {
                    card->m_dismissed = true;
                    card->Leave();
                    if (m_onDismissed)
                        m_onDismissed(id, byUser);
                    return true;
                }
            return false;
        }

        core::Array<core::RefPtr<ToastCard>> m_cards; // shown cards, oldest first, including those leaving
        core::Array<Waiting> m_waiting;
        core::Array<Incoming> m_incoming; // posted from other threads, under m_lock
        core::Mutex m_lock;               // guards m_incoming and m_lastId
        ToastId m_lastId = 0;
        usize m_maxShown = 4;
        f32 m_defaults[4] = {};
        bool m_ticking = false; // the clock's tick action is running
        core::Function<void(ToastId, const ToastCard&)> m_onPosted;
        core::Function<void(ToastId, bool)> m_onDismissed;
    };
}
