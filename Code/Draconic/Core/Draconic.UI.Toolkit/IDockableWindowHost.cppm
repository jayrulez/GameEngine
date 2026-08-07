// Draconic UI Toolkit - :idockable_window_host partition
//
// Bridge between the docking system (UI layer) and the application (framework layer). Abstracts whether
// dockable windows are real OS windows or virtual (PopupLayer) overlays. Ported from
// Sedulous.UI.Toolkit/src/Docking/IDockableWindowHost.bf. Beef `delegate void(View)` -> Function<void(View*)>;
// Beef `out float` params -> `f32&` out-params.
//
// Coordinate frame: all positions are logical pixels relative to the main editor window's client-area
// top-left; sizes are logical pixels.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui.toolkit:idockable_window_host;

import draconic.foundation;
import draconic.ui;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Implement in the Application class and assign to DockManager's DockableWindowHost.
    class IDockableWindowHost
    {
    public:
        virtual ~IDockableWindowHost() = default;

        /// Whether this host supports creating real OS windows.
        [[nodiscard]] virtual bool SupportsOSWindows() = 0;

        /// Create a real OS window to host the given dockable window view. `onCloseRequested` is called
        /// when the OS window close button is clicked.
        virtual void CreateDockableWindow(View* dockableWindow, f32 width, f32 height, f32 x, f32 y,
                                          Function<void(View*)> onCloseRequested = {}) = 0;

        /// Destroy the OS window hosting the given dockable window view.
        virtual void DestroyDockableWindow(View* dockableWindow) = 0;

        /// Move the OS window hosting the given dockable window (logical px, main-window-relative).
        virtual void MoveDockableWindow(View* dockableWindow, f32 x, f32 y) = 0;

        /// Resize and reposition the OS window hosting the given dockable window.
        virtual void ResizeDockableWindow(View* dockableWindow, f32 x, f32 y, f32 width,
                                          f32 height) = 0;

        /// Read the OS window's current logical-px position (main-window-relative) AND size. Returns false
        /// when the view isn't OS-hosted.
        [[nodiscard]] virtual bool TryGetDockableWindowBounds(View* dockableWindow, f32& x, f32& y,
                                                              f32& width, f32& height) = 0;

        /// Current desktop-global mouse position in logical px.
        virtual void GetGlobalMousePosition(f32& globalX, f32& globalY) = 0;
    };
}
