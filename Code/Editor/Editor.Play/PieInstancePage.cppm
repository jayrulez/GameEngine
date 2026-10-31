// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Play - :pie_page_interface partition.
//
// IPieInstancePage is what one play-in-editor instance, a Game tab, offers the PIE tools. Beyond
// what the scene tools read through IGameScenePage (its id, whether it runs, and its scene), it
// covers starting and stopping the run, the run's state, a capture of what it renders, the scene
// and game script it runs, a scripted input timeline in place of the viewport's, and the
// resolution it draws at. The Game page publishes it (EditorPage::Service<IPieInstancePage>).
module;
#include "Core/Prelude.h"

export module editor.play:pie_page_interface;

import foundation.core;
import foundation.input;
import foundation.scene;
import editor.core;
import editor.scene;          // ViewportCapture, IGameScenePage
import :game_resolution;      // GameResolutionChoice

using namespace foundation::core;

export namespace editor
{
    /// Where a PIE instance's startup script stands.
    enum class PieScriptState : u8
    {
        None,    ///< the project has no startup script, or the run has not started one
        Running,
        Faulted, ///< it stopped on its own (IPieInstancePage::ScriptFault says why)
    };

    class IPieInstancePage : public IGameScenePage
    {
    public:

        /// Asks for a cook and starts the run once the cook is idle; nothing while running.
        virtual void Play() = 0;
        /// Stops the run; the tab stays open.
        virtual void Stop() = 0;

        /// Whether Play was asked for and the run is waiting on the cook.
        [[nodiscard]] virtual bool IsStarting() const noexcept = 0;
        /// The scene the run is in, empty when it has none (a script that owns boot, between
        /// levels).
        [[nodiscard]] virtual StringView SceneName() const noexcept = 0;
        /// Seconds of frames since the run started, unscaled. It keeps counting while the game
        /// pauses its scene at time scale 0 (behind a menu, which still works) and stands still
        /// while the debugger holds the run. Scripted input is timed by it.
        [[nodiscard]] virtual f64 RunTime() const noexcept = 0;
        /// Frames rendered since the run started.
        [[nodiscard]] virtual u64 FrameCount() const noexcept = 0;
        /// The startup script's state, and the reason a faulted one stopped.
        [[nodiscard]] virtual PieScriptState ScriptState() const noexcept = 0;
        [[nodiscard]] virtual StringView ScriptFault() const noexcept = 0;

        /// Asks for the viewport's next rendered frame, after the game's own UI and overlays, as
        /// a PNG at `path` (its directory must exist), replacing a pending request.
        virtual void RequestViewportCapture(StringView path) = 0;
        /// The latest request's state, as it advances frame by frame.
        [[nodiscard]] virtual const ViewportCapture& LastViewportCapture() const noexcept = 0;

        /// Reads a property of the running game script; NotFound without a script or with no
        /// such property.
        [[nodiscard]] virtual Result<Variant> GetScriptProperty(StringView name) const = 0;

        /// Plays `source` into this instance in place of the viewport's input, with its time zero
        /// now; the page advances it each frame by the run's time. A script already playing is
        /// dropped.
        virtual void BeginScriptedInput(UniquePtr<foundation::input::ScriptedInputSource> source) = 0;
        /// Ends the script: one frame releases everything it holds, then the viewport's input
        /// returns. Stopping the run ends it at once.
        virtual void EndScriptedInput() = 0;
        /// Whether a script is installed (until the viewport's input is back).
        [[nodiscard]] virtual bool IsScripted() const noexcept = 0;

        /// The resolutions the tab's dropdown offers, in its order and up to date (an export
        /// preset or preview resolution added since is included).
        [[nodiscard]] virtual Span<const GameResolutionChoice> ResolutionChoices() = 0;
        /// The chosen entry's key.
        [[nodiscard]] virtual StringView ResolutionKey() const noexcept = 0;
        /// Chooses the entry with this key as the dropdown does: it is shown there, kept for the
        /// project and applied at once. False when no entry has the key.
        virtual bool SelectResolution(StringView key) = 0;
        /// Whether the tab previews a phone held upright (its Phone toggle).
        [[nodiscard]] virtual bool PhonePreview() const noexcept = 0;
        /// Sets the Phone toggle as a click does: it is shown, kept for the project and applied at once.
        virtual void SelectPhonePreview(bool on) = 0;

    protected:
        ~IPieInstancePage() = default;
    };
}
