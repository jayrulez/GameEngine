// Draconic::EditorApp - :ui_page partition.
//
// UIEditorPage: the UI-side extension of the headless editor::EditorPage - a page that owns a
// draconic.ui content view (docked as a closable center tab by EditorApplication) and receives
// the app's frame hooks so it can drive per-page work (viewport binding, camera, offscreen
// rendering). Every IEditorPageFactory registered into THIS app's context must produce
// UIEditorPages (EditorApplication static_casts on open) - the headless EditorPage stays UI-free
// for core tests, this is the one seam where pages meet the UI/runtime.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.editor.app:ui_page;

import draconic.foundation;
import draconic.graphics;
import draconic.runtime.client;
import draconic.ui;
import draconic.editor.core;

using namespace draconic::foundation;

export namespace draconic::editor::app
{
    class UIEditorPage : public draconic::editor::EditorPage
    {
    public:
        /// The view docked into the center document area (owned by the page).
        [[nodiscard]] virtual draconic::ui::View* ContentView() = 0;

        /// Per-frame hook, after the UI laid out (viewport rects are current).
        virtual void OnUpdate(draconic::runtime::IApplicationHost& host, f32 dt)
        {
            (void)host;
            (void)dt;
        }

        /// Per-window render hook, before the UI draws (offscreen content the UI then samples).
        virtual void OnRenderWindow(draconic::runtime::IApplicationHost& host,
                                    draconic::graphics::FrameContext& frame)
        {
            (void)host;
            (void)frame;
        }

        /// After the scene renderer's EndRendering (targets are COMPOSED): overlays that
        /// draw ON the page's offscreen content (the Game tab's screen-tier UI).
        virtual void OnAfterSceneRender(draconic::runtime::IApplicationHost& host,
                                        draconic::graphics::FrameContext& frame)
        {
            (void)host;
            (void)frame;
        }

        /// Called right before the page is removed - release GPU/scene resources while the
        /// device and window are still alive.
        virtual void OnClose() {}
    };
}
