// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :log_view partition.
//
// LogView: the Console panel content - a log view on foundation.ui, with category display
// (core logs carry categories). A
// header row - a chip per level (its colour, its name, how many lines it holds; on shows them), a
// search field matching anywhere in a line ignoring case (an x clears it), and a Clear icon - over
// a recycled ListView of level-colored rows; bounded entry count; auto-scroll to the newest entry.
// Fed once per frame by
// EditorApplication draining the EditorLogBuffer. Rows select like any list (click, Ctrl click,
// Shift click, Ctrl+A), and Ctrl+C or the context menu's Copy puts the selected rows on the
// clipboard, oldest first, one per line.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:log_view;

import foundation.core;
import foundation.ui;
import :editor_icons; // the search, clear-field and clear-console glyphs

using namespace foundation::core;

export namespace editor::app
{
    namespace ui = foundation::ui;

    class LogView : public ui::ViewGroup
    {
        RTTI_OBJECT(LogView, ui::ViewGroup)
    public:
        /// Display buckets (core Trace+Debug fold into Debug; Error+Fatal into Error).
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

        LogView()
        {
            auto column = MakeRef<ui::FlexLayout>(MemoryAllocator());
            column->Direction = ui::Orientation::Vertical;

            // The header row: [level chips] ........ [search: icon, field, x] [clear].
            auto toolbar = MakeRef<ui::FlexLayout>(MemoryAllocator());
            toolbar->Direction = ui::Orientation::Horizontal;
            toolbar->Spacing = 4.0f;
            toolbar->Padding = ui::Thickness{6, 3};
            toolbar->AlignItems = ui::Align::Center;
            ui::LayoutStyle center;
            center.AlignSelf = ui::Align::Center;
            EditorIcons& icons = EditorIcons::Get(); // null drawables before Initialize (tests)
            for (usize i = 0; i < kBucketCount; ++i)
            {
                toolbar->AddView(MakeLevelChip(static_cast<Bucket>(i)).Get(), center);
            }
            {
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                toolbar->AddView(MakeRef<ui::View>(MemoryAllocator()).Get(), grow); // the gap
            }
            auto magnifier = MakeRef<ui::DrawableView>(
                MemoryAllocator(), ui::DrawablePtr(icons.search.Get()), 14.0f, 14.0f);
            magnifier->KeepAspect = true;
            toolbar->AddView(magnifier.Get(), center);
            m_searchEdit = MakeRef<ui::EditText>(MemoryAllocator());
            m_searchEdit->SetPlaceholder(u8"Filter lines");
            m_searchEdit->TooltipText = String(u8"Show only the lines containing this (any case)");
            m_searchEdit->OnTextChanged.Add([this](ui::EditText* edit) { SetSearch(edit->Text()); });
            {
                // It grows with the panel up to a comfortable width and gives way in a narrow
                // one, so the header never runs past the panel's edge.
                ui::LayoutStyle field;
                field.FlexGrow = 1.0f;
                field.MinWidth = ui::Unit::Dp(80.0f);
                field.MaxWidth = ui::Unit::Dp(260.0f);
                field.AlignSelf = ui::Align::Center;
                toolbar->AddView(m_searchEdit.Get(), field);
            }
            m_clearSearch = MakeRef<ui::IconButton>(MemoryAllocator(), icons.close.Get(), 10.0f);
            m_clearSearch->TooltipText = String(u8"Clear the filter");
            m_clearSearch->Visibility = ui::VisibilityValue::Hidden; // keeps its place: no jump
            m_clearSearch->OnClick.Add(
                [this](ui::ButtonBase*)
                {
                    m_searchEdit->SetText(u8"");
                    SetSearch(u8"");
                });
            toolbar->AddView(m_clearSearch.Get(), center);
            auto clear = MakeRef<ui::IconButton>(MemoryAllocator(), icons.remove.Get(), 16.0f);
            clear->TooltipText = String(u8"Clear the console");
            clear->OnClick.Add([this](ui::ButtonBase*) { Clear(); });
            toolbar->AddView(clear.Get(), center);
            {
                ui::LayoutStyle row;
                row.Width = ui::SizeSpec::Match();
                row.Height = ui::SizeSpec::Fixed(ui::Unit::Dp(28.0f));
                column->AddView(toolbar.Get(), row);
            }
            {
                // A hairline under the header (the theme's separator), so it reads as chrome.
                auto rule = MakeRef<ui::Separator>(MemoryAllocator());
                ui::LayoutStyle line;
                line.Width = ui::SizeSpec::Match();
                column->AddView(rule.Get(), line);
            }
            UpdateCounts();

            // The entry list (recycled rows), selecting many.
            m_adapter = MakeUnique<Adapter>(MemoryAllocator(), *this);
            m_list = MakeRef<EntryList>(MemoryAllocator(), *this);
            m_list->ItemHeight.SetValue(20.0f);
            m_list->Padding = ui::Thickness{8, 4}; // the lines off the panel's edges and the header
            m_list->Selection.Mode = ui::SelectionMode::Multiple;
            m_list->SetAdapter(m_adapter.Get());
            m_list->OnItemRightClicked.Add([this](i32, f32 x, f32 y) { ShowContextMenu(x, y); });
            m_list->OnBackgroundRightClicked.Add([this](f32 x, f32 y) { ShowContextMenu(x, y); });
            {
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                column->AddView(m_list.Get(), grow);
            }

            AddView(column.Get());
        }

        ~LogView() override { m_list->SetAdapter(nullptr); }

        /// Says a copy happened (the app's toast): the number of lines copied.
        Function<void(usize)> OnCopied;

        /// True while the newest entry is kept in view.
        bool AutoScroll = true;

        /// Cap on retained entries (oldest trimmed).
        usize MaxEntries = 1000;

        void AddEntry(LogLevel level, StringView category, StringView message)
        {
            Entry entry;
            entry.bucket = BucketOf(level);
            entry.text = String(u8"[");
            entry.text += category;
            entry.text += u8"] ";
            entry.text += message;
            ++m_counts[static_cast<usize>(entry.bucket)];
            m_entries.PushBack(Move(entry));

            // Trim to cap: indices into m_entries shift, so the filter is rebuilt below, and the
            // selection moves up by the visible rows that went (a trimmed selected row is gone).
            bool trimmed = false;
            i32 trimmedVisible = 0;
            while (m_entries.Size() > MaxEntries)
            {
                if (Shows(m_entries[0]))
                {
                    ++trimmedVisible;
                }
                --m_counts[static_cast<usize>(m_entries[0].bucket)];
                m_entries.RemoveAt(0);
                trimmed = true;
            }

            if (trimmed)
            {
                m_list->Selection.ShiftIndices(0, -trimmedVisible);
                RebuildFilter();
            }
            else if (Shows(m_entries.Back()))
            {
                m_filtered.PushBack(m_entries.Size() - 1);
                m_adapter->NotifyDataSetChanged();
            }
            UpdateCounts();
            ScrollToNewest();
        }

        void Clear()
        {
            m_entries.Clear();
            m_filtered.Clear();
            m_list->Selection.ClearSelection();
            m_adapter->NotifyDataSetChanged();
            for (usize& count : m_counts)
            {
                count = 0;
            }
            UpdateCounts();
        }

        /// A level's chip in the header (on shows its lines), and the count it shows.
        [[nodiscard]] ui::ToggleButton* LevelChip(Bucket bucket) const noexcept
        {
            return m_chips[static_cast<usize>(bucket)];
        }
        [[nodiscard]] usize LevelCount(Bucket bucket) const noexcept
        {
            return m_counts[static_cast<usize>(bucket)];
        }
        [[nodiscard]] StringView LevelCountText(Bucket bucket) const noexcept
        {
            const ui::Label* label = m_countLabels[static_cast<usize>(bucket)];
            return label != nullptr ? label->Text.Value().AsView() : StringView{};
        }
        /// The search field's clear button (shown only while the field holds text).
        [[nodiscard]] ui::IconButton* ClearSearchButton() const noexcept
        {
            return m_clearSearch.Get();
        }

        /// Selects every visible row.
        void SelectAll()
        {
            if (!m_filtered.IsEmpty())
            {
                m_list->Selection.SelectRange(0, static_cast<i32>(m_filtered.Size()) - 1);
            }
        }

        [[nodiscard]] usize SelectedCount() const noexcept
        {
            return m_list->Selection.SelectedCount();
        }
        /// The entry list: its selection, and the keys it takes (the tests drive both).
        [[nodiscard]] ui::ListView& List() noexcept { return *m_list; }

        /// The selected rows' text, oldest first, one per line.
        [[nodiscard]] String SelectedText() const
        {
            Array<i32> positions;
            for (i32 position : m_list->Selection.SelectedPositions())
            {
                if (position >= 0 && static_cast<usize>(position) < m_filtered.Size())
                {
                    positions.PushBack(position);
                }
            }
            positions.Sort([](i32 a, i32 b) { return a < b; });
            String text;
            for (i32 position : positions)
            {
                if (!text.IsEmpty())
                {
                    text.Append(u8"\n");
                }
                text.Append(m_entries[m_filtered[static_cast<usize>(position)]].text.AsView());
            }
            return text;
        }

        /// Puts the selected rows on the system clipboard; nothing selected copies nothing.
        void CopySelection()
        {
            const String text = SelectedText();
            ui::IClipboard* clipboard = Context != nullptr ? Context->Clipboard() : nullptr;
            if (text.IsEmpty() || clipboard == nullptr)
            {
                return;
            }
            if (clipboard->SetText(text.AsView()).IsOk() && OnCopied)
            {
                OnCopied(SelectedCount());
            }
        }

        void SetBucketVisible(Bucket bucket, bool visible)
        {
            m_visible[static_cast<usize>(bucket)] = visible;
            // A filter change re-numbers the rows, so the selection goes.
            m_list->Selection.ClearSelection();
            if (ui::ToggleButton* chip = m_chips[static_cast<usize>(bucket)])
            {
                chip->IsChecked.SetSilent(visible); // keep the header in sync on programmatic calls
                chip->Invalidate();
            }
            UpdateCounts(); // an off level's chip dims
            RebuildFilter();
            ScrollToNewest();
        }

        [[nodiscard]] bool IsBucketVisible(Bucket bucket) const noexcept
        {
            return m_visible[static_cast<usize>(bucket)];
        }

        /// Shows only the lines containing `text`, ignoring case (the category is part of the
        /// line); empty shows them all. The search box calls it as the user types.
        void SetSearch(StringView text)
        {
            if (m_search.AsView() == text)
            {
                return;
            }
            m_search = String(text);
            if (m_clearSearch)
            {
                m_clearSearch->Visibility =
                    m_search.IsEmpty() ? ui::VisibilityValue::Hidden : ui::VisibilityValue::Visible;
            }
            // The rows re-number, so the selection goes.
            m_list->Selection.ClearSelection();
            RebuildFilter();
            ScrollToNewest();
        }
        [[nodiscard]] StringView Search() const noexcept { return m_search.AsView(); }

        [[nodiscard]] usize EntryCount() const noexcept { return m_entries.Size(); }
        [[nodiscard]] usize VisibleEntryCount() const noexcept { return m_filtered.Size(); }

        [[nodiscard]] StringView VisibleEntryText(usize visibleIndex) const
        {
            return m_entries[m_filtered[visibleIndex]].text.AsView();
        }

        // Fill the available space (the default ViewGroup measure wraps to children, which would
        // collapse the virtualized list); the single child column fills us.
        void OnMeasure(ui::BoxConstraints constraints) override
        {
            for (usize i = 0; i < ChildCount(); ++i)
            {
                GetChildAt(i)->Measure(constraints);
            }
            MeasuredSize = Float2{constraints.MaxWidth, constraints.MaxHeight};
        }

        void OnLayout(f32 left, f32 top, f32 width, f32 height) override
        {
            (void)left;
            (void)top;
            for (usize i = 0; i < ChildCount(); ++i)
            {
                GetChildAt(i)->Layout(0, 0, width, height);
            }
        }

    private:
        // The entry list: Ctrl+C copies the selection and Ctrl+A selects every row.
        class EntryList final : public ui::ListView
        {
        public:
            explicit EntryList(LogView& owner) : m_owner(&owner) {}

            void OnKeyDown(ui::KeyEventArgs& e) override
            {
                if (ui::HasFlag(e.Modifiers, ui::KeyModifiers::Ctrl) &&
                    !ui::HasFlag(e.Modifiers, ui::KeyModifiers::Alt))
                {
                    if (e.Key == ui::KeyCode::C)
                    {
                        m_owner->CopySelection();
                        e.Handled = true;
                        return;
                    }
                    if (e.Key == ui::KeyCode::A)
                    {
                        m_owner->SelectAll();
                        e.Handled = true;
                        return;
                    }
                }
                ui::ListView::OnKeyDown(e);
            }

        private:
            LogView* m_owner;
        };

        void ShowContextMenu(f32 localX, f32 localY)
        {
            if (Context == nullptr)
            {
                return;
            }
            auto menu = MakeRef<ui::ContextMenu>(MemoryAllocator());
            const usize count = SelectedCount();
            const String copyLabel =
                count > 1 ? Format(u8"Copy {} Lines", count) : String(u8"Copy");
            menu->AddItem(copyLabel.AsView(), [this]() { CopySelection(); }, count > 0);
            menu->AddItem(u8"Select All", [this]() { SelectAll(); }, !m_filtered.IsEmpty());
            menu->AddSeparator();
            menu->AddItem(u8"Clear", [this]() { Clear(); });
            const Float2 at = m_list->LocalToScreen(Float2{localX, localY});
            menu->Show(Context, at.x, at.y);
        }

        struct Entry
        {
            Bucket bucket = Bucket::Info;
            String text;
        };

        [[nodiscard]] static Color BucketColor(Bucket bucket) noexcept
        {
            switch (bucket)
            {
            case Bucket::Debug:
                return Color{150.0f / 255.0f, 150.0f / 255.0f, 150.0f / 255.0f, 1.0f};
            case Bucket::Info:
                return Color{80.0f / 255.0f, 180.0f / 255.0f, 255.0f / 255.0f, 1.0f};
            case Bucket::Warning:
                return Color{255.0f / 255.0f, 200.0f / 255.0f, 50.0f / 255.0f, 1.0f};
            default:
                return Color{255.0f / 255.0f, 80.0f / 255.0f, 80.0f / 255.0f, 1.0f};
            }
        }

        // Recycled row = one Label; bind sets text + level color.
        class Adapter final : public ui::ListAdapterBase
        {
        public:
            explicit Adapter(LogView& owner) : m_owner(&owner) {}

            [[nodiscard]] i32 ItemCount() const override
            {
                return static_cast<i32>(m_owner->m_filtered.Size());
            }

            [[nodiscard]] RefPtr<ui::View> CreateView(i32) override
            {
                auto label = MakeRef<ui::Label>(m_owner->MemoryAllocator());
                label->FontSize.SetValue(12.0f);
                return RefPtr<ui::View>(label.Get());
            }

            void BindView(ui::View* view, i32 position) override
            {
                auto* label = Cast<ui::Label>(view);
                if (label == nullptr)
                {
                    return;
                }
                const usize index = m_owner->m_filtered[static_cast<usize>(position)];
                const Entry& entry = m_owner->m_entries[index];
                label->SetText(entry.text.AsView());
                label->TextColor.SetValue(Optional<Color>(BucketColor(entry.bucket)));
            }

        private:
            LogView* m_owner;
        };

        // Whether a line passes the level filters and the search.
        [[nodiscard]] bool Shows(const Entry& entry) const
        {
            return IsBucketVisible(entry.bucket) &&
                   (m_search.IsEmpty() || entry.text.AsView().ContainsIgnoreCase(m_search.AsView()));
        }

        // A level's chip: a dot in the level's row colour, its name, and its line count; a toggle,
        // on while its lines show, dimmed off.
        RefPtr<ui::ToggleButton> MakeLevelChip(Bucket bucket)
        {
            static constexpr const char8_t* kNames[kBucketCount] = {u8"Debug", u8"Info",
                                                                    u8"Warning", u8"Error"};
            static constexpr const char8_t* kTips[kBucketCount] = {
                u8"Show debug and trace lines", u8"Show info lines", u8"Show warnings",
                u8"Show errors"};
            const usize i = static_cast<usize>(bucket);
            auto content = MakeRef<ui::FlexLayout>(MemoryAllocator());
            content->Direction = ui::Orientation::Horizontal;
            content->Spacing = 5.0f;
            content->AlignItems = ui::Align::Center;
            auto dotShape =
                MakeRef<ui::RoundedRectDrawable>(MemoryAllocator(), BucketColor(bucket), 4.0f);
            auto dot = MakeRef<ui::DrawableView>(MemoryAllocator(), ui::DrawablePtr(dotShape.Get()),
                                                 8.0f, 8.0f);
            content->AddView(dot.Get());
            auto name = MakeRef<ui::Label>(MemoryAllocator(), StringView(kNames[i]));
            name->FontSize.SetValue(12.0f);
            content->AddView(name.Get());
            m_nameLabels[i] = name.Get();
            auto count = MakeRef<ui::Label>(MemoryAllocator(), StringView(u8"0"));
            count->FontSize.SetValue(11.0f);
            count->TextColor.SetValue(Optional<Color>(Color{0.6f, 0.6f, 0.62f, 1.0f}));
            content->AddView(count.Get());
            m_countLabels[i] = count.Get();

            auto chip = MakeRef<ui::ToggleButton>(MemoryAllocator());
            chip->SetContent(RefPtr<ui::View>(content.Get()));
            chip->SetStyle(ui::StyleProperty::Padding, ui::Thickness{7.0f, 2.0f});
            // A quiet pill rather than the theme's accent toggle: outlined when off, a soft fill
            // when on, a touch lighter under the pointer.
            const auto pill = [this](f32 fill, f32 border)
            {
                return RefPtr<ui::Drawable>(MakeRef<ui::RoundedRectDrawable>(
                                                MemoryAllocator(), Color{1.0f, 1.0f, 1.0f, fill}, 9.0f,
                                                Color{1.0f, 1.0f, 1.0f, border}, 1.0f)
                                                .Get());
            };
            auto look = MakeRef<ui::StateListDrawable>(MemoryAllocator());
            look->Set(ui::ControlState::Normal, pill(0.0f, 0.10f));
            look->Set(ui::ControlState::Hover, pill(0.05f, 0.14f));
            look->Set(ui::ControlState::Checked, pill(0.10f, 0.16f));
            look->Set(ui::ControlState::Checked | ui::ControlState::Hover, pill(0.14f, 0.20f));
            chip->SetStyle(ui::StyleProperty::Background, RefPtr<ui::Drawable>(look.Get()));
            // A checked toggle draws its CheckedBackground first (the theme's accent): this pill too.
            chip->SetStyle(ui::StyleProperty::CheckedBackground, RefPtr<ui::Drawable>(look.Get()));
            chip->TooltipText = String(kTips[i]);
            chip->IsChecked.SetSilent(m_visible[i]);
            chip->OnCheckedChanged.Add([this, bucket](ui::ToggleButton*, bool checked)
                                       { SetBucketVisible(bucket, checked); });
            m_chips[i] = chip.Get();
            return chip;
        }

        // Each chip's count: the lines its level holds now (capped in the text, not the count).
        void UpdateCounts()
        {
            for (usize i = 0; i < kBucketCount; ++i)
            {
                if (ui::Label* label = m_countLabels[i])
                {
                    const String text = m_counts[i] > 9999
                                            ? String(u8"9999+")
                                            : Format(u8"{}", static_cast<u64>(m_counts[i]));
                    if (label->Text.Value().AsView() != text.AsView())
                    {
                        label->SetText(text.AsView());
                    }
                    // An empty level's chip reads quieter.
                    const Color quiet{0.45f, 0.45f, 0.47f, 1.0f};
                    const Color shown{0.72f, 0.72f, 0.74f, 1.0f};
                    label->TextColor.SetValue(
                        Optional<Color>(m_counts[i] == 0 || !m_visible[i] ? quiet : shown));
                }
                // The name dims with its level hidden.
                if (ui::Label* name = m_nameLabels[i])
                {
                    name->TextColor.SetValue(Optional<Color>(
                        m_visible[i] ? Color{0.9f, 0.9f, 0.92f, 1.0f} : Color{0.5f, 0.5f, 0.52f, 1.0f}));
                }
            }
        }

        void RebuildFilter()
        {
            m_filtered.Clear();
            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                if (Shows(m_entries[i]))
                {
                    m_filtered.PushBack(i);
                }
            }
            m_adapter->NotifyDataSetChanged();
        }

        void ScrollToNewest()
        {
            if (AutoScroll && !m_filtered.IsEmpty())
            {
                m_list->ScrollToPosition(static_cast<i32>(m_filtered.Size()) - 1);
            }
        }

        Array<Entry> m_entries;
        Array<usize> m_filtered; // indices into m_entries passing the filter
        bool m_visible[kBucketCount] = {true, true, true, true};
        String m_search; // what a line must contain to show; empty shows every line
        RefPtr<ui::EditText> m_searchEdit;

        UniquePtr<Adapter> m_adapter;
        RefPtr<EntryList> m_list;
        ui::ToggleButton* m_chips[kBucketCount] = {};    // borrowed (the header owns them)
        ui::Label* m_countLabels[kBucketCount] = {};     // borrowed (each chip owns its own)
        ui::Label* m_nameLabels[kBucketCount] = {};      // borrowed (each chip owns its own)
        usize m_counts[kBucketCount] = {};               // the lines each level holds now
        RefPtr<ui::IconButton> m_clearSearch;
    };

    RTTI_DEFINE_OBJECT(LogView, "rtti::editor::editor::app")
}
