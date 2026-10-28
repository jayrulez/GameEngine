// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler.Capture - :recorder partition.
///
/// A capture of the next few seconds, written as a file (Specs/profiler.md, P3): started by a key
/// or a flag in any player (or by a tool), it arms the profiler, lets the time run, waits a few
/// frames more for the last frames' GPU passes (they arrive when their frame slot comes round),
/// then writes the frames as a profile stream (.aeprofile, which the editor's panel opens) and as
/// Chrome trace JSON beside it (for Perfetto), and lets the profiler go. The host advances it after
/// each frame's end.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module foundation.profiler.capture:recorder;

import foundation.core;
import foundation.profiler;
import :chrome;

using namespace foundation::core;

export namespace foundation::profiler
{
    /// Write `source`'s frames `first` to `last` as a capture: `<base>.aeprofile`, the stream the
    /// editor's panel opens, and `<base>.json` beside it, Chrome trace JSON for Perfetto. The
    /// folder must exist. Answers the stream file's path, empty when it could not be written.
    [[nodiscard]] inline String WriteCaptureFiles(IAllocator& allocator, const IProfileSource& source, u64 first,
                                                  u64 last, StringView base, const StreamHeader& header)
    {
        String streamPath(allocator);
        AppendFormat(streamPath, u8"{}.aeprofile", base);
        {
            FileStream file(streamPath.AsView(), FileMode::Write);
            if (!file.IsValid())
            {
                LOG_WARNING(u8"Profiler", u8"capture: cannot write {}", streamPath.AsView());
                return String(allocator);
            }
            ProfileStreamWriter writer(allocator, file);
            writer.WriteHeader(header);
            for (u64 n = first; n <= last; ++n)
            {
                if (const FrameRecord* frame = source.Frame(n))
                {
                    writer.WriteFrame(source, *frame);
                }
            }
        }
        String jsonPath(allocator);
        AppendFormat(jsonPath, u8"{}.json", base);
        const String json = WriteChromeTrace(source, first, last, header.application.AsView());
        FileStream file(jsonPath.AsView(), FileMode::Write);
        if (file.IsValid())
        {
            (void)file.Write(json.Data(), json.Size());
        }
        return streamPath;
    }

    /// CaptureRecorder::kGpuGraceFrames, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr u32 kCaptureRecorderGpuGraceFrames = 4;

    class CaptureRecorder
    {
    public:
        /// Frames waited after the time is up, for the last frames' GPU passes (frames in flight).
        static constexpr u32 kGpuGraceFrames = kCaptureRecorderGpuGraceFrames;

        /// The process's recorder (the key, the flag and the tools share it). NON-inline
        /// (ProfilerCaptureImpl.cpp): one per process across shared libraries.
        [[nodiscard]] static CaptureRecorder& Get() noexcept;

        explicit CaptureRecorder(IAllocator& allocator) noexcept
            : m_allocator(&allocator), m_application(u8"Engine", allocator), m_platform(allocator), m_build(allocator),
              m_folder(allocator), m_base(allocator), m_lastCapture(allocator)
        {
        }

        /// Who records: the application (a player sets its project's name), the platform and build
        /// (the host sets them), written into each capture's header.
        [[nodiscard]] StringView Application() const noexcept { return m_application.AsView(); }
        [[nodiscard]] StringView Platform() const noexcept { return m_platform.AsView(); }
        [[nodiscard]] StringView Build() const noexcept { return m_build.AsView(); }
        void SetApplication(StringView name) { m_application = String(name, *m_allocator); }
        void SetBuild(StringView platform, StringView build)
        {
            m_platform = String(platform, *m_allocator);
            m_build = String(build, *m_allocator);
        }

        /// Capture the next `seconds` (at least one frame) into `folder` (made when missing), named
        /// after the application. False when a capture is already running.
        bool Start(Profiler& profiler, f64 seconds, StringView folder)
        {
            if (m_state != State::Idle)
            {
                return false;
            }
            m_folder = String(folder, *m_allocator);
            m_base = String(*m_allocator);
            AppendFormat(m_base, u8"{}/{}-{}", folder, m_application.AsView(), Clock::Now().Ticks());
            m_durationTicks = static_cast<u64>(Max(seconds, 0.0) * static_cast<f64>(GetTickFrequency()));
            m_startTick = GetTicks();
            m_firstFrame = profiler.CurrentFrameNumber();
            m_grace = 0;
            profiler.Arm();
            m_state = State::Recording;
            return true;
        }

        [[nodiscard]] bool Capturing() const noexcept { return m_state != State::Idle; }
        /// The last capture's stream file (its Chrome JSON beside it, ".json"); empty before one.
        [[nodiscard]] StringView LastCapture() const noexcept { return m_lastCapture.AsView(); }

        /// After each frame's end (the frame thread). True on the frame a capture was written.
        bool OnFrameEnd(Profiler& profiler)
        {
            if (m_state == State::Recording)
            {
                if (GetTicks() - m_startTick < m_durationTicks || profiler.CurrentFrameNumber() == m_firstFrame)
                {
                    return false;
                }
                m_lastFrame = profiler.CurrentFrameNumber() - 1; // the last frame that ended
                m_state = State::Waiting;
            }
            if (m_state != State::Waiting || ++m_grace < kGpuGraceFrames)
            {
                return false;
            }
            Write(profiler);
            profiler.Disarm();
            m_state = State::Idle;
            return true;
        }

    private:
        enum class State : u8
        {
            Idle,
            Recording, // the time running
            Waiting,   // for the last frames' GPU passes
        };

        void Write(Profiler& profiler)
        {
            (void)CreateDirectories(m_folder.AsView());
            const LiveProfileSource source(profiler);
            StreamHeader header;
            header.tickFrequency = source.TickFrequency();
            header.application = m_application;
            header.platform = m_platform;
            header.build = m_build;
            String written = WriteCaptureFiles(*m_allocator, source, Max(m_firstFrame, source.OldestFrame()),
                                               m_lastFrame, m_base.AsView(), header);
            if (!written.IsEmpty())
            {
                m_lastCapture = Move(written);
            }
        }

        IAllocator* m_allocator;
        String m_application;
        String m_platform;
        String m_build;
        String m_folder;
        String m_base;
        String m_lastCapture;
        State m_state = State::Idle;
        u64 m_durationTicks = 0;
        u64 m_startTick = 0;
        u64 m_firstFrame = 0;
        u64 m_lastFrame = 0;
        u32 m_grace = 0;
    };
}
