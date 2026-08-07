// Draconic::EditorApp - :log_view partition.
//
// LogView: the Console panel content (docs/design/editor.md §3.10) - Sedulous's LogView shape
// on draconic.ui, plus category display (foundation logs carry categories; Sedulous had none). A
// filter/action toolbar (per-level CheckBoxes + Clear) over a recycled ListView of level-colored
// rows; bounded entry count; auto-scroll to the newest entry. Fed once per frame by
// EditorApplication draining the EditorLogBuffer.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.editor.app:log_view;

import draconic.foundation;
import draconic.ui;

using namespace draconic::foundation;

export namespace draconic::editor::app
{
    namespace ui = draconic::ui;

    class LogView : public ui::ViewGroup
    {
        DRACONIC_OBJECT(LogView, ui::ViewGroup)
    public:
        /// Display buckets (foundation Trace+Debug fold into Debug; Error+Fatal into Error).
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
            auto column = MakeRef<ui::FlexLayout>(DefaultAllocator());
            column->Direction = ui::Orientation::Vertical;

            // Toolbar: level filters + Clear.
            auto toolbar = MakeRef<ui::FlexLayout>(DefaultAllocator());
            toolbar->Direction = ui::Orientation::Horizontal;
            toolbar->Spacing = 8.0f;
            toolbar->Padding = ui::Thickness{4, 4};
            static constexpr const char8_t* kNames[kBucketCount] = {u8"Debug", u8"Info",
                                                                    u8"Warning", u8"Error"};
            for (usize i = 0; i < kBucketCount; ++i)
            {
                auto box = MakeRef<ui::CheckBox>(DefaultAllocator(), StringView(kNames[i]), true);
                const usize bucket = i;
                box->OnCheckedChanged.Add(
                    [this, bucket](ui::CheckBox*, bool checked)
                    { SetBucketVisible(static_cast<Bucket>(bucket), checked); });
                m_filterBoxes[i] = box.Get();
                toolbar->AddView(box.Get());
            }
            auto clear = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Clear"));
            clear->OnClick.Add([this](ui::ButtonBase*) { Clear(); });
            toolbar->AddView(clear.Get());
            column->AddView(toolbar.Get());

            // The entry list (recycled rows).
            m_adapter = MakeUnique<Adapter>(DefaultAllocator(), *this);
            m_list = MakeRef<ui::ListView>(DefaultAllocator());
            m_list->ItemHeight.SetValue(20.0f);
            m_list->SetAdapter(m_adapter.Get());
            {
                auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
                grow->Grow = 1.0f;
                column->AddView(m_list.Get(), grow);
            }

            AddView(column.Get());
        }

        ~LogView() override { m_list->SetAdapter(nullptr); }

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
            m_entries.PushBack(Move(entry));

            // Trim to cap (indices into m_entries shift; rebuild the filter view below).
            bool trimmed = false;
            while (m_entries.Size() > MaxEntries)
            {
                m_entries.RemoveAt(0);
                trimmed = true;
            }

            if (trimmed)
            {
                RebuildFilter();
            }
            else if (IsBucketVisible(m_entries.Back().bucket))
            {
                m_filtered.PushBack(m_entries.Size() - 1);
                m_adapter->NotifyDataSetChanged();
            }
            ScrollToNewest();
        }

        void Clear()
        {
            m_entries.Clear();
            m_filtered.Clear();
            m_adapter->NotifyDataSetChanged();
        }

        void SetBucketVisible(Bucket bucket, bool visible)
        {
            m_visible[static_cast<usize>(bucket)] = visible;
            if (ui::CheckBox* box = m_filterBoxes[static_cast<usize>(bucket)])
            {
                box->IsChecked.SetSilent(visible); // keep the toolbar in sync on programmatic calls
            }
            RebuildFilter();
            ScrollToNewest();
        }

        [[nodiscard]] bool IsBucketVisible(Bucket bucket) const noexcept
        {
            return m_visible[static_cast<usize>(bucket)];
        }

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
                auto label = MakeRef<ui::Label>(DefaultAllocator());
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

        void RebuildFilter()
        {
            m_filtered.Clear();
            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                if (IsBucketVisible(m_entries[i].bucket))
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

        UniquePtr<Adapter> m_adapter;
        RefPtr<ui::ListView> m_list;
        ui::CheckBox* m_filterBoxes[kBucketCount] = {}; // borrowed (toolbar owns them)
    };

    DRACONIC_DEFINE_OBJECT(LogView, "draconic::editor::app")
}
