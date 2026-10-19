// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.runtime.desktop - the desktop application runner.
//
// RunApplication is the DESKTOP execution model: a blocking wall-clock loop that drives an
// IApplication (via ApplicationHost) against an IShell until either stops. It lives here -
// separate from the shell backend (foundation.shell.desktop) and from the execution-model-agnostic
// client (foundation.runtime.client) - precisely BECAUSE the loop is execution-model-specific:
// desktop blocks in a while-loop, whereas Emscripten must yield to the browser via a callback
// (its runner will be this module's sibling). The loop works entirely through the abstract IShell
// interface, so it is windowing-backend agnostic; the concrete shell is constructed by the entry
// point and passed in.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h" // the stall watchdog's report

export module foundation.runtime.desktop;

import foundation.core;
import foundation.shell;          // IShell (interface only - the concrete shell is handed in)
import foundation.graphics;       // GraphicsDevice (handed to the app)
import foundation.runtime.client; // IApplication + ApplicationHost (the runner drives these)
namespace shell = foundation::shell;

namespace core = foundation::core;
using namespace foundation::graphics; // GraphicsDevice (moved from foundation::runtime)

namespace foundation::runtime::detail
{
    // Reports that the main loop has STOPPED ADVANCING, while it is still stuck.
    //
    // Why a thread and not a per-frame timer: a timer can only report a slow frame AFTER the
    // frame ends, so the case that matters - a frame that never ends - prints nothing at all.
    // That is exactly what happened chasing the editor's minimize hang: hours of hangs with no
    // log line to show for them. This watches a heartbeat the loop bumps and speaks up DURING
    // the stall, so an unreproducible hang at least leaves a timestamped record of when it
    // started, how long it lasted, and whether the window was minimized going in.
    //
    // Deliberately knows nothing about the cause: cause-specific probes only fire when the
    // guess is right, and ours did not.
    class StallWatchdog
    {
    public:
        // `thresholdMs` before the first report, then one line per further threshold so a
        // permanent hang keeps a running duration rather than printing once and going quiet.
        explicit StallWatchdog(core::u32 thresholdMs = 5000) : m_thresholdMs(thresholdMs)
        {
            m_thread = core::Thread([this]() { Watch(); });
        }

        ~StallWatchdog()
        {
            m_stop.store(true);
            m_thread.Join();
        }

        StallWatchdog(const StallWatchdog&) = delete;
        StallWatchdog& operator=(const StallWatchdog&) = delete;

        /// Called once per loop iteration from the main thread. `minimized` is sampled HERE
        /// rather than queried by the watchdog: window state belongs to the main thread (SDL
        /// is explicit about that), and the last value before a stall is the one we want.
        void Beat(bool minimized) noexcept
        {
            m_minimized.store(minimized);
            m_beat.fetch_add(1);
        }

    private:
        void Watch()
        {
            constexpr core::u32 kPollMs = 250;
            core::u64 lastSeen = m_beat.load();
            core::u32 stalledMs = 0;
            core::u32 nextReportMs = m_thresholdMs;
            while (!m_stop.load())
            {
                core::SleepMilliseconds(kPollMs);
                const core::u64 beat = m_beat.load();
                if (beat != lastSeen)
                {
                    if (stalledMs >= m_thresholdMs)
                    {
                        LOG_WARNING(u8"Runtime",
                                    u8"main loop resumed after ~{} ms stalled (frame {})",
                                    stalledMs, beat);
                    }
                    lastSeen = beat;
                    stalledMs = 0;
                    nextReportMs = m_thresholdMs;
                    continue;
                }
                stalledMs += kPollMs;
                if (stalledMs >= nextReportMs)
                {
                    LOG_ERROR(u8"Runtime",
                              u8"main loop has not advanced for ~{} ms (frame {}, window "
                              u8"minimized={}). The UI thread is stuck INSIDE this frame.",
                              stalledMs, beat, m_minimized.load() ? 1 : 0);
                    nextReportMs += m_thresholdMs;
                }
            }
        }

        core::Atomic<core::u64> m_beat{0};
        core::Atomic<bool> m_stop{false};
        core::Atomic<bool> m_minimized{false};
        core::u32 m_thresholdMs;
        core::Thread m_thread;
    };
}

export namespace foundation::runtime
{
    // Desktop runner: block-loop the app against the shell until either stops, clamped to
    // maxFrameTime. APP_MAIN calls it on desktop; returns the app's exit code. The loop is paced
    // by presenting (vsync); an iteration that presented nothing (every window minimized) rests
    // a moment instead of spinning a core.
    inline constexpr core::u32 kIdleRestMilliseconds = 10;

    inline int RunApplication(IApplication& app, shell::IShell& shell,
                              GraphicsDevice* graphics = nullptr)
    {
        ApplicationHost host(core::DefaultAllocator()); // process composition root
        host.Start(app, &shell, graphics);
        // Always on: the hangs worth catching are the ones nobody can reproduce on demand.
        // Costs an atomic increment per frame and one 250 ms-polling thread.
        detail::StallWatchdog watchdog;
        core::TimePoint previous = core::Clock::Now();
        while (shell.IsRunning() && host.IsRunning())
        {
            const shell::IWindowManager* windows = shell.WindowManager();
            const shell::IWindow* mainWindow =
                windows != nullptr ? windows->MainWindow() : nullptr;
            watchdog.Beat(mainWindow != nullptr && mainWindow->IsMinimized());
            shell.ProcessEvents();
            const core::TimePoint now = core::Clock::Now();
            core::f32 dt = (now - previous).AsSecondsF();
            previous = now;
            if (dt > host.Settings().maxFrameTime)
            {
                dt = host.Settings().maxFrameTime;
            }
            host.Tick(dt);
            if (host.IdleLastTick())
            {
                core::SleepMilliseconds(kIdleRestMilliseconds);
            }
        }
        host.Stop();
        return host.ExitCode();
    }
}
