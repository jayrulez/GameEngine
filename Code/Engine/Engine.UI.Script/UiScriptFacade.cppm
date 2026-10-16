// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::UI.Script - :facade partition
//
// The `ui` script facade (bound lowercase as `ui` via the ScriptName alias) - the SCREEN tier surfaced
// to scripts. `ui` addresses the one app-wide screen-tier RootView: its finders search that root
// recursively (`ui.findLabel("score")`), `ui.root()`/`ui.top()` return group handles to narrow a
// search, and `ui.push/pop/replace/clear/back` drive the gamekit ScreenStack. Resolves its OWN
// per-context service (kUiScreenScriptService), filled by the app with the live screen root + stack +
// a cooked-document instantiator; unwired -> safe no-ops (null handles, no-op pushes).
//
// Scene-tier UI is intentionally NOT here (a run has many live scenes): scene UI
// rides a UICanvasComponent. There is no scene.ui script root.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module engine.ui.script:facade;

import foundation.core;
import engine.domain;
import foundation.ui;
import foundation.ui.gamekit;
import foundation.script;
import :types;

using namespace foundation::core;

export namespace engine::uiscript
{
    // The per-context service the `ui` facade resolves (distinct from every other runtime service).
    inline constexpr StringView kUiScreenScriptService = u8"ui.screen.runtime";

    // Installed as a per-context script service; the `ui` facade resolves it. The app fills it, backed
    // by the UISubsystem's screen tier + the resource manager. Null members = safe no-ops.
    struct UiScreenScriptBinding
    {
        foundation::ui::gamekit::ScreenStack* stack = nullptr;  // the stack over the screen root (tier-owned)
        // The calling script's run's stack, when the host gives each run its own screen tier (the
        // editor's Game tabs, UISubsystem::ScreensFor); unset, every script uses `stack`.
        Function<foundation::ui::gamekit::ScreenStack*(const void* run)> stackForRun;
        // Instantiate a cooked UIDocument (by guid) into a live view tree. The app supplies this
        // (resource manager + MarkupLoader); nil -> push returns a null Screen.
        Function<RefPtr<foundation::ui::View>(const Guid&)> instantiate;
    };

    inline void InstallUiScreenScriptService(foundation::script::IScriptContext& context,
                                             UiScreenScriptBinding& binding)
    {
        context.SetService(kUiScreenScriptService, &binding);
    }
    inline void ClearUiScreenScriptService(foundation::script::IScriptContext& context)
    {
        context.SetService(kUiScreenScriptService, nullptr);
    }

    /// ui.*: the screen tier for scripts. Finders search the app-wide screen root; push/pop/replace
    /// drive the ScreenStack. Static facade (resolve per call via CurrentScriptContext), bound to
    /// scripts as `ui` via ScriptName. Unwired -> null handles + no-op stack ops.
    class Ui final : public Object
    {
        RTTI_OBJECT(Ui, Object)
    public:
        [[nodiscard]] static UiScreenScriptBinding* Resolve();
        /// The stack the calling script's screens live on: its run's, or the shared one.
        [[nodiscard]] static foundation::ui::gamekit::ScreenStack* Stack(UiScreenScriptBinding* b);

        // Roots / narrowing.
        [[nodiscard]] static ViewGroup root();  // the screen-tier root as a group handle
        [[nodiscard]] static Screen top();       // the top screen on the stack (null if empty)
        [[nodiscard]] static i32 count();        // number of screens on the stack

        // Convenience finders (search from the screen root).
        [[nodiscard]] static View find(String name);
        [[nodiscard]] static Label findLabel(String name);
        [[nodiscard]] static Button findButton(String name);
        [[nodiscard]] static ProgressBar findProgressBar(String name);
        [[nodiscard]] static Slider findSlider(String name);
        [[nodiscard]] static RichText findRichText(String name);
        [[nodiscard]] static TextBox findTextBox(String name);
        [[nodiscard]] static Image findImage(String name);
        [[nodiscard]] static ViewGroup findGroup(String name);

        // Screen management.
        [[nodiscard]] static Screen push(Guid document);
        static void pop();
        [[nodiscard]] static Screen replace(Guid document);
        static void clear();
        static bool back(); // pop the top unless it is the last screen; true if popped
    };

    // Registers the reflected view-handle types + the `ui` facade (bound as `ui`) with the global
    // registry and the behavior-prelude name list. Idempotent; call before a script manager is created
    // (the run host / cook builder do). Mirrors the other RegisterXxxScriptFacade entry points.
    void RegisterUiScriptSurface();
}

export namespace engine::uiscript
{
    /// This domain's declaration (engine-composition.md D4): what it brings to a scene, to
    /// reflection, to the script surface and which resource modules come with it. Defined in the
    /// implementation unit (one instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& UiScriptDomain() noexcept;
}
