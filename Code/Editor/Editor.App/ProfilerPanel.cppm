// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :profiler_panel partition.
//
// ProfilerPanel: the editor's Profiler (X7 of Documentation/Specs/editor-gui.md), a tab in the
// page area that floats as any page does. It records the editor process, every page, viewport
// preview and Play-in-editor run in one frame, and says so: a game's own numbers come from its
// player (the overlay, or a capture from it opened here). Its frame is markup
// (Data/Editor/Profiler.xml); its views are Profiler.GUI's, all on one focus.
//
// Record starts copying the store's frames into the panel's own recording and Stop ends it, so
// stopping, the choices and a longer history than the store keeps never touch the store another
// viewer reads (a PIE game's overlay). While Live, the newest frame is chosen as it comes; a
// choice on the strip stops that, and Live brings it back (from a capture, the editor's own
// recording again). Stop on spike ends the recording at the first frame over its time, that frame
// chosen. Save writes the recording as a capture (.aeprofile and its Chrome trace JSON) to
// <user-data>/profiles; Open shows a capture from there, a player's from a Deck or a phone copied in.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:profiler_panel;

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.profiler;
import foundation.profiler.gui;
import foundation.profiler.capture;
import editor.core; // NoticeKind
import :picker;
import :path_picker_dialog;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;
    namespace profiler = foundation::profiler;
    namespace pgui = foundation::profiler::gui;

    /// What the panel records and where its captures go: the process's store and recorder (a
    /// test's own), the folder (made when missing), the picker it opens a capture through, and who
    /// tells the user how a save or an open went (the editor's toasts; `folder`, when not empty, the
    /// one a save went to, for the toast to open).
    struct ProfilerPanelSeams
    {
        profiler::Profiler* store = nullptr;
        profiler::CaptureRecorder* recorder = nullptr; // the platform and build a saved capture names
        Function<String()> folder;
        Function<void(PickerRequest)> pick;
        Function<void(editor::NoticeKind, StringView message, StringView folder)> notify;
    };

    /// ProfilerPanel::kPath, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr const char8_t* kProfilerPanelPath = u8"Editor/Profiler.xml";

    class ProfilerPanel final : public pgui::IProfileFocusClient
    {
    public:
        /// Its frame, under the data root.
        static constexpr const char8_t* kPath = kProfilerPanelPath;
        /// The budget's choices, the combo's order: 30, 60 and 120 fps, then none.
        static constexpr f32 kBudgets[] = {1000.0f / 30.0f, 1000.0f / 60.0f, 1000.0f / 120.0f, 0.0f};
        /// Stop on spike's times, the combo's order.
        static constexpr f32 kSpikes[] = {25.0f, 33.3f, 50.0f, 100.0f};

        ProfilerPanel(IAllocator& allocator, ProfilerPanelSeams seams, RefPtr<gui::Node> content)
            : m_allocator(allocator), m_seams(Move(seams)), m_content(Move(content)), m_live(*m_seams.store),
              m_recording(allocator), m_counterPlots(allocator)
        {
            m_focus = MakeRef<pgui::ProfileFocus>(allocator, allocator);
            m_axis = MakeRef<pgui::FrameAxis>(allocator, allocator);
            m_focus->Join(this);
            if (!m_content)
                return;
            ProfilerPanel* self = this;
            m_strip = Find<pgui::FrameStrip>(u8"profiler-strip");
            m_timeline = Find<pgui::ProfileTimeline>(u8"profiler-timeline");
            m_table = Find<pgui::ScopeTable>(u8"profiler-table");
            m_detail = Find<pgui::ScopeDetail>(u8"profiler-detail");
            m_counters = Find<gui::Panel>(u8"profiler-counters");
            if (m_strip != nullptr)
            {
                m_strip->SetProfileFocus(m_focus);
                m_strip->SetFrameAxis(m_axis);
                m_strip->OnFramesPicked = [self]() { self->SetFollowing(false); };
            }
            if (m_timeline != nullptr)
                m_timeline->SetProfileFocus(m_focus);
            if (m_table != nullptr)
                m_table->SetProfileFocus(m_focus);
            if (m_detail != nullptr)
                m_detail->SetProfileFocus(m_focus);

            m_record = Find<gui::ToolButton>(u8"profiler-record");
            if (m_record != nullptr)
                m_record->SetOnToggled([self](bool on) { on ? self->Start() : self->Stop(); });
            m_liveButton = Find<gui::ToolButton>(u8"profiler-live");
            if (m_liveButton != nullptr)
                m_liveButton->SetOnToggled([self](bool on) { on ? self->ShowLive() : self->SetFollowing(false); });
            if (auto* open = Find<gui::ToolButton>(u8"profiler-open"))
                open->SetOnClick([self]() { self->PickCapture(); });
            if (auto* clear = Find<gui::ToolButton>(u8"profiler-clear"))
                clear->SetOnClick([self]() { self->Clear(); });
            m_save = Find<gui::ToolButton>(u8"profiler-save");
            if (m_save != nullptr)
                m_save->SetOnClick([self]() { (void)self->SaveRecording(); });
            if (auto* budget = Find<gui::ComboBox>(u8"profiler-budget"))
            {
                budget->SetOnSelectionChanged([self](i32 index) { self->SetBudgetChoice(index); });
                SetBudgetChoice(budget->GetSelectedIndex());
            }
            m_spike = Find<gui::ToolButton>(u8"profiler-spike");
            if (m_spike != nullptr)
                m_spike->SetOnToggled([self](bool on) { self->SetStopOnSpike(on); });
            if (auto* spike = Find<gui::ComboBox>(u8"profiler-spike-ms"))
            {
                spike->SetOnSelectionChanged([self](i32 index) { self->SetSpikeChoice(index); });
                SetSpikeChoice(spike->GetSelectedIndex());
            }
            if (auto* longest = Find<gui::Button>(u8"profiler-longest"))
                longest->SetOnClick([self]() { self->ChooseLongest(); });
            if (auto* median = Find<gui::Button>(u8"profiler-median"))
                median->SetOnClick([self]() { self->ChooseMedian(); });
            if (auto* zoom = Find<gui::Button>(u8"profiler-zoom-longest"))
                zoom->SetOnClick([self]() { self->ZoomToLongest(); });
            if (auto* filter = Find<gui::TextField>(u8"profiler-filter"))
                filter->SetOnTextChanging([self](StringView text)
                                          {
                                              if (self->m_table != nullptr)
                                                  self->m_table->SetFilter(text);
                                          });
            ShowLive();
        }
        ProfilerPanel(const ProfilerPanel&) = delete;
        ProfilerPanel& operator=(const ProfilerPanel&) = delete;
        ~ProfilerPanel() override
        {
            if (m_recordingNow)
                m_seams.store->Disarm();
            m_focus->Leave(this);
            m_focus->SetSource(nullptr);
        }

        [[nodiscard]] RefPtr<gui::Node> Content() const { return m_content; }
        [[nodiscard]] pgui::ProfileFocus& Focus() noexcept { return *m_focus; }
        [[nodiscard]] pgui::FrameAxis& Axis() noexcept { return *m_axis; }

        /// Starts recording the editor process afresh (arming the store, counted).
        void Start()
        {
            if (m_recordingNow)
                return;
            ShowLive();
            m_recording.Begin(m_live);
            m_seams.store->Arm();
            m_recordingNow = true;
            m_axis->Fit(0, 0); // fitting again: it follows the recording from here
            SyncToggles();
            Refresh();
        }
        /// Stops: the frames recorded stay to read and save; the store is let go.
        void Stop()
        {
            if (!m_recordingNow)
                return;
            m_recording.Pull(m_live, 0); // the rest, as they are
            m_seams.store->Disarm();
            m_recordingNow = false;
            SyncToggles();
            Refresh();
        }
        [[nodiscard]] bool IsRecording() const noexcept { return m_recordingNow; }
        /// Drops the frames recorded; a recording goes on from the next frame (the old panel's
        /// Clear). The page shows the recording again, nothing chosen.
        void Clear()
        {
            m_recording.Clear();
            if (m_capture.Get() != nullptr)
            {
                m_capture = nullptr;
                m_capturePath = String(m_allocator);
                SetSource(&m_recording);
            }
            m_focus->SetFrames(profiler::FrameRange{});
            m_axis->Fit(0, 0);
            SetFollowing(m_recordingNow);
            Refresh();
        }
        [[nodiscard]] bool IsFollowing() const noexcept { return m_following; }

        /// Each frame: the store's newly settled frames taken while recording; Stop on spike
        /// stops at the first frame over its time.
        void Update()
        {
            if (!m_recordingNow)
                return;
            const u32 before = m_recording.FrameCount();
            const u64 newest = m_recording.NewestFrame();
            const u64 next = m_recording.HasFrames() ? newest + 1 : 0;
            m_recording.Pull(m_live);
            if (m_recording.FrameCount() == before && m_recording.NewestFrame() == newest)
                return;
            if (m_stopOnSpike && m_recording.HasFrames())
                for (u64 n = Max(next, m_recording.OldestFrame()); n <= m_recording.NewestFrame(); ++n)
                    if (const profiler::FrameRecord* frame = m_recording.Frame(n);
                        frame != nullptr && profiler::TicksToMs(frame->endTick - frame->beginTick, m_recording.TickFrequency()) > m_spikeMs)
                    {
                        Stop();
                        SetFollowing(false);
                        m_focus->SetFrame(n);
                        SetStatus(Format(u8"Stopped on frame {}: {} ms, over {} ms", n,
                                         FormatFixed(profiler::TicksToMs(frame->endTick - frame->beginTick, m_recording.TickFrequency()), 2).AsView(),
                                         FormatFixed(m_spikeMs, 0).AsView())
                                      .AsView());
                        return;
                    }
            Refresh();
        }

        /// Writes the recording as a capture; answers the .aeprofile's path (empty: nothing to
        /// save, or it could not be written, which the log says).
        String SaveRecording()
        {
            if (m_capture.Get() != nullptr || !m_recording.HasFrames())
            {
                Notify(editor::NoticeKind::Info, u8"Nothing to save: record first");
                return String(m_allocator);
            }
            const String folder = m_seams.folder();
            (void)CreateDirectories(folder.AsView());
            const String base = Format(u8"{}/Editor-{}", folder.AsView(), Clock::Now().Ticks());
            profiler::StreamHeader header;
            header.tickFrequency = m_recording.TickFrequency();
            header.application = String(u8"Editor", m_allocator);
            if (m_seams.recorder != nullptr)
            {
                header.platform = String(m_seams.recorder->Platform(), m_allocator);
                header.build = String(m_seams.recorder->Build(), m_allocator);
            }
            String path = profiler::WriteCaptureFiles(m_allocator, m_recording, m_recording.OldestFrame(), m_recording.NewestFrame(),
                                                      base.AsView(), header);
            if (path.IsEmpty())
                Notify(editor::NoticeKind::Error, Format(u8"Could not save the recording to {} (see the Console)", folder.AsView()).AsView());
            else
            {
                Notify(editor::NoticeKind::Success, Format(u8"Saved {} frames", m_recording.FrameCount()).AsView(), folder.AsView());
                LOG_INFO(u8"Profiler", u8"saved the editor's recording to {}", path.AsView());
            }
            return path;
        }

        /// Shows the capture file at `path` (recording stops), its newest frame chosen. False when
        /// it cannot be read.
        bool OpenCapture(StringView path)
        {
            FileStream file(path, FileMode::Read);
            auto capture = MakeUnique<profiler::ProfileCapture>(m_allocator, m_allocator);
            if (!file.IsValid() || !capture->Read(file))
            {
                Notify(editor::NoticeKind::Error, Format(u8"Could not read {}", path).AsView());
                return false;
            }
            Stop();
            m_capture = Move(capture);
            m_capturePath = String(path, m_allocator);
            SetFollowing(false);
            SetSource(m_capture.Get());
            if (m_capture->HasFrames())
                m_focus->SetFrame(m_capture->NewestFrame());
            return true;
        }

        /// The editor's own recording, following its newest frame.
        void ShowLive()
        {
            if (m_capture.Get() != nullptr || m_focus->Source() == nullptr)
            {
                m_capture = nullptr;
                m_capturePath = String(m_allocator);
                SetSource(&m_recording);
            }
            SetFollowing(true);
            Refresh();
        }
        [[nodiscard]] bool ViewingCapture() const noexcept { return m_capture.Get() != nullptr; }
        [[nodiscard]] const profiler::ProfileRecording& Recording() const noexcept { return m_recording; }
        [[nodiscard]] StringView StatusText() const noexcept { return m_statusText.AsView(); }

        /// Follows the newest frame as it comes, or not (a choice on the strip stops it).
        void SetFollowing(bool following)
        {
            m_following = following;
            SyncToggles();
            if (following)
                Refresh();
        }

        /// The budget in ms (0: none) and Stop on spike's time.
        void SetBudgetChoice(i32 index)
        {
            const usize at = index >= 0 && static_cast<usize>(index) < sizeof(kBudgets) / sizeof(kBudgets[0]) ? static_cast<usize>(index) : 1;
            if (m_strip != nullptr)
                m_strip->SetBudgetMs(kBudgets[at]);
        }
        void SetSpikeChoice(i32 index)
        {
            const usize at = index >= 0 && static_cast<usize>(index) < sizeof(kSpikes) / sizeof(kSpikes[0]) ? static_cast<usize>(index) : 2;
            m_spikeMs = kSpikes[at];
        }
        void SetStopOnSpike(bool on)
        {
            m_stopOnSpike = on;
            if (m_spike != nullptr && m_spike->IsChecked() != on)
                m_spike->SetChecked(on);
        }
        [[nodiscard]] bool StopsOnSpike() const noexcept { return m_stopOnSpike; }

        /// The longest and the median frame held, chosen.
        void ChooseLongest() { ChooseFrame(pgui::LongestFrame(*m_focus->Source(), pgui::AllFrames(m_focus->Source()))); }
        void ChooseMedian() { ChooseFrame(pgui::MedianFrame(*m_focus->Source(), pgui::AllFrames(m_focus->Source()), m_allocator)); }
        /// The chosen scope's longest call in the frames chosen, on the timeline.
        void ZoomToLongest()
        {
            u64 begin = 0;
            u64 end = 0;
            if (m_detail != nullptr && m_timeline != nullptr && m_detail->LongestCall(begin, end))
                m_timeline->ZoomTo(static_cast<f64>(begin), static_cast<f64>(end));
        }

        /// The plots made for the source's counters (one each, in their order).
        [[nodiscard]] usize CounterPlotCount() const noexcept { return m_counterPlots.Size(); }

        void OnProfileFramesChanged() override { UpdateFrameText(); }
        void OnProfileChoiceChanged() override { UpdateScopeNote(); }

    private:
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

        void SetSource(const profiler::IProfileSource* source)
        {
            m_axis->Fit(0, 0); // every frame of the new source, following it as it grows
            m_focus->SetSource(source);
            SyncCounters();
            if (m_save != nullptr)
                m_save->SetEnabled(m_capture.Get() == nullptr);
            UpdateStatus();
        }

        // New frames: every view reads them again; while following, the newest is chosen.
        void Refresh()
        {
            m_focus->SourceGrew();
            SyncCounters();
            const profiler::IProfileSource* source = m_focus->Source();
            if (m_following && source != nullptr && source->HasFrames())
                m_focus->SetFrame(source->NewestFrame());
            UpdateStatus();
            UpdateFrameText();
        }

        void ChooseFrame(profiler::FrameRange frame)
        {
            if (frame.empty)
                return;
            SetFollowing(false);
            m_focus->SetFrames(frame);
        }

        // A plot per counter the source has, under the strip.
        void SyncCounters()
        {
            const profiler::IProfileSource* source = m_focus->Source();
            const usize wanted = source != nullptr ? source->Counters().Size() : 0;
            if (m_counters == nullptr || wanted == m_counterPlots.Size())
                return;
            while (m_counterPlots.Size() > wanted)
            {
                m_counterPlots.Back()->RemoveFromParent();
                m_counterPlots.PopBack();
            }
            while (m_counterPlots.Size() < wanted)
            {
                RefPtr<pgui::CounterPlot> plot = MakeRef<pgui::CounterPlot>(m_allocator);
                plot->SetProfileFocus(m_focus);
                plot->SetFrameAxis(m_axis);
                plot->SetCounter(static_cast<u32>(m_counterPlots.Size()));
                ProfilerPanel* self = this;
                plot->OnFramesPicked = [self]() { self->SetFollowing(false); };
                m_counters->AddChild(plot.Get());
                m_counterPlots.PushBack(Move(plot));
            }
        }

        void SyncToggles()
        {
            if (m_record != nullptr && m_record->IsChecked() != m_recordingNow)
                m_record->SetChecked(m_recordingNow);
            const bool live = m_following && m_capture.Get() == nullptr;
            if (m_liveButton != nullptr && m_liveButton->IsChecked() != live)
                m_liveButton->SetChecked(live);
        }

        void UpdateStatus()
        {
            String text(m_allocator);
            if (m_capture.Get() != nullptr)
            {
                const profiler::StreamHeader& header = m_capture->Header();
                text = Format(u8"{}: {} frames of {} ({} {})", m_capturePath.AsView(), m_capture->FrameCount(), header.application.AsView(),
                              header.platform.AsView(), header.build.AsView());
            }
            else if (m_recordingNow)
                text = Format(u8"Recording the editor process: {} frames", m_recording.FrameCount());
            else if (m_recording.HasFrames())
                text = Format(u8"The editor process: {} frames", m_recording.FrameCount());
            else
                text = String(u8"The editor process (every page, preview and Play-in-editor run): Record to start");
            SetStatus(text.AsView());
        }
        // How a save or an open went, said once (a toast), not left in the bar: the bar's label
        // says what is shown, and stays short.
        void Notify(editor::NoticeKind kind, StringView message, StringView folder = {})
        {
            if (m_seams.notify)
                m_seams.notify(kind, message, folder);
        }
        void SetStatus(StringView text)
        {
            m_statusText = String(text, m_allocator);
            if (auto* label = Find<gui::Label>(u8"profiler-source"))
                label->SetText(text);
        }

        // The strip's bar: the frames chosen and their times.
        void UpdateFrameText()
        {
            auto* frameLabel = Find<gui::Label>(u8"profiler-frame");
            auto* timesLabel = Find<gui::Label>(u8"profiler-frame-times");
            const profiler::IProfileSource* source = m_focus->Source();
            const profiler::FrameRange frames = m_focus->Frames();
            String frame;
            String times;
            if (source != nullptr && !frames.empty)
            {
                frame = frames.first == frames.last ? Format(u8"Frame {}", frames.first) : Format(u8"Frames {} to {}", frames.first, frames.last);
                Array<profiler::FrameTime> collected(m_allocator);
                profiler::CollectFrameTimes(*source, frames, collected);
                if (collected.Size() == 1)
                    times = collected[0].hasGpu ? Format(u8"CPU {} ms   GPU {} ms", FormatFixed(collected[0].cpuMs, 2).AsView(), FormatFixed(collected[0].gpuMs, 2).AsView())
                                                : Format(u8"CPU {} ms", FormatFixed(collected[0].cpuMs, 2).AsView());
                else if (!collected.IsEmpty())
                {
                    const profiler::FrameWindowStats stats = profiler::StatsOf(collected.AsSpan(), m_allocator);
                    times = Format(u8"CPU mean {}   median {}   max {} ms", FormatFixed(stats.cpu.mean, 2).AsView(), FormatFixed(stats.cpu.median, 2).AsView(),
                                   FormatFixed(stats.cpu.max, 2).AsView());
                }
            }
            if (frameLabel != nullptr)
                frameLabel->SetText(frame.AsView());
            if (timesLabel != nullptr)
                timesLabel->SetText(times.AsView());
            UpdateScopeNote();
        }
        // The table's bar: what it covers.
        void UpdateScopeNote()
        {
            auto* note = Find<gui::Label>(u8"profiler-scope-note");
            if (note == nullptr)
                return;
            const profiler::IProfileSource* source = m_focus->Source();
            const profiler::FrameRange frames = m_focus->Frames();
            String text;
            if (source != nullptr && !frames.empty)
            {
                text = frames.first == frames.last ? Format(u8"Frame {}", frames.first) : Format(u8"Frames {} to {}", frames.first, frames.last);
                const pgui::MarkedSpan span = m_focus->Span();
                if (span.any)
                    AppendFormat(text, u8", a span of {} ms", FormatFixed(profiler::TicksToMs(span.endTick - span.beginTick, source->TickFrequency()), 2).AsView());
            }
            note->SetText(text.AsView());
        }

        void PickCapture()
        {
            if (!m_seams.pick)
                return;
            const String folder = m_seams.folder();
            (void)CreateDirectories(folder.AsView());
            const String extensions[] = {String(u8".aeprofile")};
            PickerRequest request = PathPicker::Request(m_allocator, folder.AsView(), Span<const String>(extensions, 1u));
            ProfilerPanel* self = this;
            request.OnPicked = [self, folder](StringView picked) { (void)self->OpenCapture(PathJoin(folder.AsView(), picked).AsView()); };
            m_seams.pick(Move(request));
        }

        IAllocator& m_allocator;
        ProfilerPanelSeams m_seams;
        RefPtr<gui::Node> m_content;
        profiler::LiveProfileSource m_live;
        profiler::ProfileRecording m_recording;
        UniquePtr<profiler::ProfileCapture> m_capture; // shown instead of the recording, when open
        String m_capturePath;
        RefPtr<pgui::ProfileFocus> m_focus;
        RefPtr<pgui::FrameAxis> m_axis;
        bool m_recordingNow = false;
        bool m_following = true;
        bool m_stopOnSpike = false;
        f32 m_spikeMs = 50.0f;
        String m_statusText;

        // Borrowed from the content.
        pgui::FrameStrip* m_strip = nullptr;
        pgui::ProfileTimeline* m_timeline = nullptr;
        pgui::ScopeTable* m_table = nullptr;
        pgui::ScopeDetail* m_detail = nullptr;
        gui::Panel* m_counters = nullptr;
        gui::ToolButton* m_record = nullptr;
        gui::ToolButton* m_liveButton = nullptr;
        gui::ToolButton* m_save = nullptr;
        gui::ToolButton* m_spike = nullptr;
        Array<RefPtr<pgui::CounterPlot>> m_counterPlots;
    };
}
