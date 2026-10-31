// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::DefaultApp - the `engine.defaultapp:profiler_overlay` partition: an application's
// profiler overlay, drawn on the GUI.
// Whether an app has one, and what turns it on, is the app's to decide; the GUI only gives it a
// place to draw, its overlay layer (GuiSubsystem::PushOverlay), above every screen and run, where
// a tree that takes no pointer leaves the game playable under it. DefaultApplication owns one (F3,
// --profile-overlay, the web page's ?profile-overlay); another app can own its own.
module;
#include "Core/Prelude.h"
#include "Profiler/Profiler.h"

export module engine.defaultapp:profiler_overlay;

import foundation.core;
import foundation.profiler;
import foundation.profiler.gui;
import engine.gui;

using namespace foundation::core;

export namespace engine::runtime
{
    class ProfilerOverlayHost
    {
    public:
        /// `profiler` is the store it shows and arms while shown (a test's own, or the process's).
        explicit ProfilerOverlayHost(IAllocator& allocator,
                                     foundation::profiler::Profiler& profiler = foundation::profiler::Profiler::Get())
            : m_allocator(&allocator), m_profiler(&profiler)
        {
        }
        ~ProfilerOverlayHost() { Hide(); }
        ProfilerOverlayHost(const ProfilerOverlayHost&) = delete;
        ProfilerOverlayHost& operator=(const ProfilerOverlayHost&) = delete;

        /// Show it on `gui`'s overlay layer and arm the profiler (counted: a capture or the
        /// editor's profiler keeps its own arm). Nothing in a build without the profiler (Shipping).
        void Show(engine::gui::GuiSubsystem& gui)
        {
#if BUILD_PROFILING
            if (m_gui != nullptr)
            {
                return;
            }
            if (m_overlay.Get() == nullptr)
            {
                m_source = MakeUnique<foundation::profiler::LiveProfileSource>(*m_allocator, *m_profiler);
                m_focus = MakeRef<foundation::profiler::gui::ProfileFocus>(*m_allocator, *m_allocator);
                m_overlay = MakeRef<foundation::profiler::gui::ProfilerOverlay>(*m_allocator);
                m_overlay->SetProfileFocus(m_focus);
                m_focus->SetSource(m_source.Get());
            }
            gui.PushOverlay(*m_overlay);
            m_profiler->Arm();
            m_gui = &gui;
            m_overlay->Refresh();
#else
            (void)gui;
#endif
        }

        /// Take it down and let the profiler go. Before the GUI it shows on shuts down.
        void Hide()
        {
            if (m_gui == nullptr)
            {
                return;
            }
            m_gui->RemoveOverlay(*m_overlay);
            m_profiler->Disarm();
            m_gui = nullptr;
        }

        void Toggle(engine::gui::GuiSubsystem& gui)
        {
            if (Shown())
            {
                Hide();
            }
            else
            {
                Show(gui);
            }
        }

        [[nodiscard]] bool Shown() const noexcept { return m_gui != nullptr; }

        /// Each frame, before the GUI draws: the overlay reads the frames that ended since.
        void Refresh()
        {
            if (m_gui != nullptr)
            {
                m_overlay->Refresh();
            }
        }

        /// The overlay once it has been shown (tests, an app placing it in another corner).
        [[nodiscard]] foundation::profiler::gui::ProfilerOverlay* Overlay() noexcept { return m_overlay.Get(); }

    private:
        [[maybe_unused]] IAllocator* m_allocator; // read only where the profiler is built in (not Shipping)
        foundation::profiler::Profiler* m_profiler;
        engine::gui::GuiSubsystem* m_gui = nullptr; // while shown
        UniquePtr<foundation::profiler::LiveProfileSource> m_source;
        RefPtr<foundation::profiler::gui::ProfileFocus> m_focus;
        RefPtr<foundation::profiler::gui::ProfilerOverlay> m_overlay;
    };
}
