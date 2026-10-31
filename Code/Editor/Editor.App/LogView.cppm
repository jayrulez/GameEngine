// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :log_view partition.
//
// LogView: the Console panel. Its frame is markup (Data/Editor/Console.xml): a chip per level
// (Debug, which also holds trace lines; Info; Warnings; Errors, which also holds fatal lines)
// showing how many lines it holds and toggling whether they show; a search matching anywhere in a
// line, ignoring case, cleared by its x; Clear; then the lines on a virtualised list, each with its
// time since the editor started and `[Category] message` in its level's colour. The newest line is
// followed until the user scrolls up, and again once they are back at the bottom. Rows select as
// in any list (click, Ctrl, Shift, Ctrl+A); Ctrl+C or the menu's Copy puts the selected lines on
// the clipboard, oldest first, one per line. The entries are capped (the oldest trimmed); a
// selection is by entry, so it survives a trim. It is fed once a frame from the editor's log
// buffer.
module;
#include "Core/Prelude.h"

export module editor.app:log_view;

import foundation.core;
import foundation.gui;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    /// LogView::kMaxEntries, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr usize kLogViewMaxEntries = 1000;

    /// LogView::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kLogViewPath = u8"Editor/Console.xml";

    class LogView
    {
    public:
        /// Its frame's markup path, under the data root.
        static constexpr const char8_t* kPath = kLogViewPath;

        /// The chips' levels: Debug holds trace and debug lines; Error holds fatal ones.
        enum class Bucket : u8
        {
            Debug,
            Info,
            Warning,
            Error
        };
        static constexpr usize kBucketCount = 4;
        [[nodiscard]] static Bucket BucketOf(LogLevel level) noexcept
        {
            switch (level)
            {
            case LogLevel::Trace:
            case LogLevel::Debug:
                return Bucket::Debug;
            case LogLevel::Info:
                return Bucket::Info;
            case LogLevel::Warning:
                return Bucket::Warning;
            default:
                return Bucket::Error;
            }
        }

        /// Cap on the lines kept (the oldest trimmed).
        static constexpr usize kMaxEntries = kLogViewMaxEntries;

        LogView(IAllocator& allocator, RefPtr<gui::Node> content)
            : m_allocator(allocator), m_content(Move(content)), m_entries(allocator), m_shown(allocator), m_model(*this)
        {
            if (!m_content)
                return;
            LogView* self = this;
            m_list = Find<gui::ListView>(u8"console-list");
            if (m_list != nullptr)
            {
                gui::RowFactory rows;
                rows.Create = [self](u32) { return self->MakeRow(); };
                rows.Bind = [self](gui::Node& row, const gui::ModelIndex& index) { self->BindRow(row, index); };
                m_list->SetRowFactory(Move(rows));
                m_list->SetModel(&m_model);
                // Handle Ctrl+C before the list's own keys; the menu offers Copy, Select All and Clear.
                (void)m_list->AddEventListener(gui::EventType::KeyDown, [self](const gui::Event& e)
                                               {
                                                   const auto& key = static_cast<const gui::KeyEvent&>(e);
                                                   if (static_cast<gui::KeyCode>(key.KeyCode) == gui::KeyCode::C && key.Modifiers == gui::KeyModCtrl)
                                                   {
                                                       self->CopySelection();
                                                       e.Handled = true;
                                                   }
                                               },
                                               true);
                (void)m_list->AddEventListener(gui::EventType::ContextMenu, [self](const gui::Event& e)
                                               {
                                                   self->ShowMenu(static_cast<const gui::MouseEvent&>(e).Position);
                                                   e.Handled = true;
                                               });
            }
            const char8_t* const chips[kBucketCount] = {u8"console-debug", u8"console-info", u8"console-warnings", u8"console-errors"};
            for (usize i = 0; i < kBucketCount; ++i)
            {
                m_chips[i] = Find<gui::Button>(StringView(chips[i]));
                if (m_chips[i] != nullptr)
                {
                    const Bucket bucket = static_cast<Bucket>(i);
                    m_chips[i]->SetOnClick([self, bucket]() { self->SetBucketVisible(bucket, !self->IsBucketVisible(bucket)); });
                }
            }
            if (auto* search = Find<gui::TextField>(u8"console-search"))
                search->SetOnTextChanging([self](StringView text) { self->SetSearch(text); });
            if (auto* clearSearch = Find<gui::Button>(u8"console-clear-search"))
                clearSearch->SetOnClick([self]()
                                        {
                                            if (auto* field = self->Find<gui::TextField>(u8"console-search"))
                                                field->SetText(StringView{});
                                            self->SetSearch(StringView{});
                                        });
            if (auto* clear = Find<gui::Button>(u8"console-clear"))
                clear->SetOnClick([self]() { self->Clear(); });
            ShowChrome();
        }
        LogView(const LogView&) = delete;
        LogView& operator=(const LogView&) = delete;
        ~LogView()
        {
            if (m_list != nullptr)
                m_list->SetModel(nullptr);
        }

        [[nodiscard]] RefPtr<gui::Node> Content() const { return m_content; }
        /// Called with how many lines a copy put on the clipboard (for the application's toast).
        void SetOnCopied(Function<void(usize)> copied) { m_onCopied = Move(copied); }

        /// Adds one line at the end; past the cap, the oldest are trimmed.
        void AddEntry(LogLevel level, StringView category, StringView message, Duration time = {})
        {
            const bool follow = IsAtBottom();
            Entry entry;
            entry.level = level;
            entry.text = category.IsEmpty() ? String(message) : Format(u8"[{}] {}", category, message);
            entry.time = time;
            entry.id = m_nextId++;
            ++m_counts[static_cast<usize>(BucketOf(level))];
            m_entries.PushBack(Move(entry));
            bool reset = false;
            while (m_entries.Size() > kMaxEntries)
            {
                --m_counts[static_cast<usize>(BucketOf(m_entries[0].level))];
                m_entries.RemoveAt(0);
                reset = true;
            }
            if (reset)
                Refilter();
            else if (Shows(m_entries.Back()))
            {
                m_shown.PushBack(m_entries.Size() - 1);
                m_model.Inserted(static_cast<i32>(m_shown.Size()) - 1);
            }
            ShowChrome();
            if (follow && m_list != nullptr && !m_shown.IsEmpty())
                m_list->ScrollToItem(static_cast<i32>(m_shown.Size()) - 1);
        }
        void Clear()
        {
            m_entries.Clear();
            for (usize& count : m_counts)
                count = 0;
            Refilter();
            ShowChrome();
        }

        [[nodiscard]] usize EntryCount() const noexcept { return m_entries.Size(); }
        [[nodiscard]] usize ShownCount() const noexcept { return m_shown.Size(); }
        [[nodiscard]] usize CountOf(Bucket bucket) const noexcept { return m_counts[static_cast<usize>(bucket)]; }
        /// A shown line's text ("[Category] message").
        [[nodiscard]] StringView ShownText(usize row) const { return row < m_shown.Size() ? m_entries[m_shown[row]].text.AsView() : StringView{}; }

        [[nodiscard]] bool IsBucketVisible(Bucket bucket) const noexcept { return m_visible[static_cast<usize>(bucket)]; }
        void SetBucketVisible(Bucket bucket, bool visible)
        {
            if (m_visible[static_cast<usize>(bucket)] == visible)
                return;
            m_visible[static_cast<usize>(bucket)] = visible;
            Refilter();
            ShowChrome();
        }
        /// Shows only the lines holding `text`, ignoring case (the category is part of a line);
        /// empty shows them all.
        void SetSearch(StringView text)
        {
            if (m_search.AsView() == text)
                return;
            m_search = String(text);
            Refilter();
            ShowChrome();
        }

        /// The selected lines, oldest first, one per line.
        [[nodiscard]] String SelectedText() const
        {
            String text;
            if (m_list == nullptr)
                return text;
            for (usize row = 0; row < m_shown.Size(); ++row)
                if (m_list->IsItemSelected(static_cast<i32>(row)))
                {
                    if (!text.IsEmpty())
                        text += u8"\n";
                    text += m_entries[m_shown[row]].text.AsView();
                }
            return text;
        }
        /// Copies the selected lines to the system clipboard; with nothing selected it copies nothing.
        void CopySelection()
        {
            const String text = SelectedText();
            gui::EventDispatcher* dispatcher = m_content ? m_content->GetEventDispatcher() : nullptr;
            if (text.IsEmpty() || dispatcher == nullptr || dispatcher->GetClipboard() == nullptr)
                return;
            dispatcher->GetClipboard()->SetText(text.AsView());
            if (m_onCopied)
                m_onCopied(m_list != nullptr ? m_list->GetSelectionCount() : 0);
        }
        /// The list (its selection, for a test).
        [[nodiscard]] gui::ListView* List() const noexcept { return m_list; }

    private:
        struct Entry
        {
            LogLevel level = LogLevel::Info;
            String text;
            Duration time;
            u64 id = 0;
        };

        // The shown lines as the list's model; a line's id is its entry's, so a selection follows
        // the entry.
        class Model final : public gui::IModel
        {
        public:
            explicit Model(LogView& view) : m_view(view) {}
            [[nodiscard]] usize RowCount(const gui::ModelIndex& parent = {}) const override { return parent.IsValid() ? 0 : m_view.m_shown.Size(); }
            [[nodiscard]] usize ColumnCount() const override { return 1; }
            [[nodiscard]] gui::Variant Data(const gui::ModelIndex& index, gui::ModelRole role = gui::ModelRole::Display) const override
            {
                if (role != gui::ModelRole::Display || index.Row < 0 || static_cast<usize>(index.Row) >= m_view.m_shown.Size())
                    return gui::Variant{};
                return gui::Variant(m_view.m_entries[m_view.m_shown[static_cast<usize>(index.Row)]].text.AsView());
            }
            [[nodiscard]] gui::ItemId IdOf(const gui::ModelIndex& index) const override
            {
                return index.Row >= 0 && static_cast<usize>(index.Row) < m_view.m_shown.Size() ? m_view.m_entries[m_view.m_shown[static_cast<usize>(index.Row)]].id : 0;
            }
            [[nodiscard]] gui::ModelIndex IndexOfId(gui::ItemId id) const override
            {
                for (usize row = 0; row < m_view.m_shown.Size(); ++row)
                    if (m_view.m_entries[m_view.m_shown[row]].id == id)
                        return gui::ModelIndex{static_cast<i32>(row), 0};
                return {};
            }
            void Inserted(i32 row) { NotifyRowsInserted(gui::ModelIndex{}, row, 1); }
            void Reset() { NotifyReset(); }

        private:
            LogView& m_view;
        };

        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            return m_content ? Cast<T>(FindIn(*m_content, id)) : nullptr;
        }
        static gui::Node* FindIn(gui::Node& node, StringView id)
        {
            if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindIn(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        [[nodiscard]] bool Shows(const Entry& entry) const
        {
            return m_visible[static_cast<usize>(BucketOf(entry.level))] && (m_search.IsEmpty() || entry.text.AsView().ContainsIgnoreCase(m_search.AsView()));
        }
        void Refilter()
        {
            m_shown.Clear();
            for (usize i = 0; i < m_entries.Size(); ++i)
                if (Shows(m_entries[i]))
                    m_shown.PushBack(i);
            m_model.Reset();
        }
        // Whether the list is at the bottom, or has nothing to scroll, so a new line is followed.
        [[nodiscard]] bool IsAtBottom() const
        {
            if (m_list == nullptr)
                return true;
            return m_list->GetScrollOffset().y >= m_list->GetScrollRange().y - kFollowSlack;
        }

        // Updates the chips' counts and states, the search's x, and the empty line.
        void ShowChrome()
        {
            static constexpr StringView kNames[kBucketCount] = {u8"Debug", u8"Info", u8"Warnings", u8"Errors"};
            for (usize i = 0; i < kBucketCount; ++i)
                if (gui::Button* chip = m_chips[i])
                {
                    const usize count = m_counts[i];
                    chip->SetText(count == 0 ? String(kNames[i]).AsView()
                                  : count > 9999 ? Format(u8"{} 9999+", kNames[i]).AsView()
                                                 : Format(u8"{} {}", kNames[i], count).AsView());
                    if (m_visible[i])
                        chip->AddClass(u8"checked");
                    else
                        chip->RemoveClass(u8"checked");
                }
            if (auto* clearSearch = Find<gui::Button>(u8"console-clear-search"))
                clearSearch->SetVisible(!m_search.IsEmpty()); // kept in place so the field does not jump
            if (auto* empty = Find<gui::Label>(u8"console-empty"))
            {
                empty->SetDisplayed(m_shown.IsEmpty());
                empty->SetText(m_entries.IsEmpty() ? StringView(u8"No messages yet.") : StringView(u8"No line matches."));
            }
        }

        [[nodiscard]] RefPtr<gui::Node> MakeRow()
        {
            RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
            row->AddClass(u8"log-row");
            RefPtr<gui::Label> time = MakeRef<gui::Label>(m_allocator);
            time->AddClass(u8"log-time");
            time->AddClass(u8"mono");
            RefPtr<gui::Label> text = MakeRef<gui::Label>(m_allocator);
            text->AddClass(u8"log-text");
            row->AddChild(time.Get());
            row->AddChild(text.Get());
            return RefPtr<gui::Node>(row);
        }
        void BindRow(gui::Node& node, const gui::ModelIndex& index)
        {
            auto* row = Cast<gui::UIWidget>(&node);
            if (row == nullptr || index.Row < 0 || static_cast<usize>(index.Row) >= m_shown.Size() || row->ChildCount() < 2)
                return;
            const Entry& entry = m_entries[m_shown[static_cast<usize>(index.Row)]];
            if (auto* time = Cast<gui::Label>(row->GetChildAt(0)))
                time->SetText(TimeText(entry.time).AsView());
            if (auto* text = Cast<gui::Label>(row->GetChildAt(1)))
                text->SetText(entry.text.AsView());
            static constexpr StringView kLevels[] = {u8"debug", u8"warning", u8"error"};
            for (StringView level : kLevels)
                row->RemoveClass(level);
            if (BucketOf(entry.level) == Bucket::Debug)
                row->AddClass(u8"debug");
            else if (entry.level == LogLevel::Warning)
                row->AddClass(u8"warning");
            else if (BucketOf(entry.level) == Bucket::Error)
                row->AddClass(u8"error");
        }

        // Formats the time since the editor started as hh:mm:ss.
        [[nodiscard]] static String TimeText(Duration time)
        {
            const i64 seconds = static_cast<i64>(time.AsSeconds());
            String text;
            const i64 parts[] = {seconds / 3600, (seconds / 60) % 60, seconds % 60};
            for (usize i = 0; i < 3; ++i)
            {
                if (i > 0)
                    text += u8":";
                if (parts[i] < 10)
                    text += u8"0";
                text += Format(u8"{}", parts[i]).AsView();
            }
            return text;
        }

        void ShowMenu(Float2 at)
        {
            if (m_list == nullptr)
                return;
            m_menu = MakeRef<gui::Menu>(m_allocator);
            LogView* self = this;
            gui::MenuItem* copy = m_menu->AddItem(u8"Copy", [self]() { self->CopySelection(); });
            copy->SetShortcut(u8"Ctrl+C");
            copy->SetEnabled(m_list->GetSelectionCount() > 0);
            (void)m_menu->AddItem(u8"Select All", [self]() { self->m_list->SelectAll(); });
            m_menu->AddSeparator();
            (void)m_menu->AddItem(u8"Clear", [self]() { self->Clear(); });
            m_menu->Open(*m_list, at);
        }

        static constexpr f32 kFollowSlack = 4.0f; // this close to the bottom counts as at it

        IAllocator& m_allocator;
        RefPtr<gui::Node> m_content;
        gui::ListView* m_list = nullptr;
        gui::Button* m_chips[kBucketCount] = {};
        RefPtr<gui::Menu> m_menu;
        Array<Entry> m_entries;
        Array<usize> m_shown; // the entries shown, by index into m_entries
        usize m_counts[kBucketCount] = {};
        bool m_visible[kBucketCount] = {true, true, true, true};
        String m_search;
        u64 m_nextId = 1;
        Function<void(usize)> m_onCopied;
        Model m_model;
    };
}
