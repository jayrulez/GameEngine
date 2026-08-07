// Draconic::RuntimeClient - :embedded_host partition.
//
// EmbeddedApplicationHost: the editor-embedding adapter (runtime-host.md v3, Sedulous
// EditorApplicationHost lineage). An IApplication programs against IApplicationHost and
// runs UNCHANGED whether hosted standalone or inside the editor: this adapter routes
// Ctx() to an EMBEDDED runtime Context (owned by the embedder, distinct from the outer
// host's editor-app context) while sharing the outer host's shell and REAL graphics
// device (deviation from Sedulous's Graphics=null, which forced it to re-point subsystem
// device/window after Configure). The embedded app renders into viewport textures, so
// MainRenderWindow is null and window open/close are refused. "Exit" from inside the
// embedded app means "stop the play session" - the embedder supplies that meaning via
// the exit handler (deferred to frame end by the embedder, never torn down mid-callback).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.runtime.client:embedded_host;

import draconic.foundation;
import draconic.shell;
import draconic.graphics;
import draconic.runtime;
import :app;

namespace foundation = draconic::foundation;
using namespace draconic::shell;
using namespace draconic::graphics;

export namespace draconic::runtime
{
    class EmbeddedApplicationHost final : public IApplicationHost
    {
    public:
        /// `outer` = the real host (shell/graphics are borrowed from it);
        /// `runtimeContext` = the embedded context the hosted app configures/runs in.
        EmbeddedApplicationHost(IApplicationHost& outer, Context& runtimeContext)
            : m_outer(&outer), m_runtimeContext(&runtimeContext)
        {
        }

        [[nodiscard]] Context& Ctx() noexcept override { return *m_runtimeContext; }
        [[nodiscard]] IShell* Shell() noexcept override { return m_outer->Shell(); }
        [[nodiscard]] GraphicsDevice* Graphics() noexcept override { return m_outer->Graphics(); }

        // The embedded app has no OS window - it renders into the embedder's viewport
        // texture (the Game tab). Attach-to-main-window code must tolerate null, exactly
        // as it must for headless runs.
        [[nodiscard]] RenderWindow* MainRenderWindow() noexcept override { return nullptr; }
        RenderWindow* OpenWindow(const WindowSettings&, const RenderWindowDesc&) override
        {
            DRACONIC_LOG_WARNING(u8"Runtime", u8"embedded app requested an OS window - refused");
            return nullptr;
        }
        void CloseWindow(RenderWindow*) override {}

        /// The embedder decides what "exit" means (stop the play session, deferred to
        /// frame end). Without a handler the request is logged and dropped.
        void SetExitHandler(foundation::Function<void(int)> handler) { m_onExit = foundation::Move(handler); }
        void RequestExit(int code = 0) override
        {
            if (m_onExit)
            {
                m_onExit(code);
                return;
            }
            DRACONIC_LOG_WARNING(u8"Runtime", u8"embedded app requested exit({}) - no handler",
                                 code);
        }

    private:
        IApplicationHost* m_outer;
        Context* m_runtimeContext;
        foundation::Function<void(int)> m_onExit;
    };
}
