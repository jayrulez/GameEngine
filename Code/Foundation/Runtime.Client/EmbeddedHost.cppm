// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Runtime.Client - :embedded_host partition.
//
// EmbeddedApplicationHost: the editor-embedding adapter (Sedulous
// EditorApplicationHost lineage). An IApplication programs against IApplicationHost and
// runs UNCHANGED whether hosted standalone or inside the editor: this adapter routes
// Subsystems() to an EMBEDDED runtime SubsystemManager (owned by the embedder, distinct from the outer
// host's editor-app context) while sharing the outer host's shell and REAL graphics
// device (deviation from Sedulous's Graphics=null, which forced it to re-point subsystem
// device/window after Configure). The embedded app renders into viewport textures, so
// MainRenderWindow is null and window open/close are refused. "Exit" from inside the
// embedded app means "stop the play session" - the embedder supplies that meaning via
// the exit handler (deferred to frame end by the embedder, never torn down mid-callback).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module foundation.runtime.client:embedded_host;

import foundation.core;
import foundation.shell;
import foundation.graphics;
import foundation.runtime;
import :app;

namespace core = foundation::core;
using namespace foundation::shell;
using namespace foundation::graphics;

export namespace foundation::runtime
{
    class EmbeddedApplicationHost final : public IApplicationHost
    {
    public:
        /// `outer` = the real host (shell/graphics are borrowed from it);
        /// `subsystems` = the embedded subsystem manager the hosted app configures/runs in.
        EmbeddedApplicationHost(IApplicationHost& outer, SubsystemManager& subsystems)
            : m_outer(&outer), m_subsystems(&subsystems)
        {
        }

        [[nodiscard]] SubsystemManager& Subsystems() noexcept override { return *m_subsystems; }
        [[nodiscard]] IShell* Shell() noexcept override { return m_outer->Shell(); }
        [[nodiscard]] GraphicsDevice* Graphics() noexcept override { return m_outer->Graphics(); }

        // The embedded app has no OS window - it renders into the embedder's viewport
        // texture (the Game tab). Attach-to-main-window code must tolerate null, exactly
        // as it must for headless runs.
        [[nodiscard]] RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        RenderWindow* OpenWindow(const WindowSettings&, const RenderWindowDesc&) override
        {
            LOG_WARNING(u8"Runtime", u8"embedded app requested an OS window - refused");
            return nullptr;
        }
        void CloseWindow(RenderWindow*) override {}

        /// The embedder decides what "exit" means (stop the play session, deferred to
        /// frame end). Without a handler the request is logged and dropped.
        void SetExitHandler(core::Function<void(int)> handler) { m_onExit = core::Move(handler); }
        void RequestExit(int code = 0) override
        {
            if (m_onExit)
            {
                m_onExit(code);
                return;
            }
            LOG_WARNING(u8"Runtime", u8"embedded app requested exit({}) - no handler",
                                 code);
        }

    private:
        IApplicationHost* m_outer;
        SubsystemManager* m_subsystems;
        core::Function<void(int)> m_onExit;
    };
}
