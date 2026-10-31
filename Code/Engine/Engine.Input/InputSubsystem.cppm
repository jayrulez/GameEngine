// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Input - the `engine.input` module.
//
// The runtime hookup: owns the ActionRuntime + the device provider and evaluates once per
// frame in Update (before scenes tick - Subsystem registration order puts input ahead of
// the scene subsystem in every assembly that adds it first). The provider seam is the
// play-in-editor story: the player passes the shell's devices, the editor's Game tab will
// pass its viewport's gated InputSurface facades.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module engine.input;

export import :fitted_input; // FittedInputSource: a game's pointer in its render pixels

import foundation.core;
import engine.domain;
import foundation.shell;
import foundation.runtime;
import foundation.run; // the runs whose action runtimes the UI reaches
import foundation.script;
import foundation.input;

using namespace foundation::core;
using namespace foundation::input;

export namespace engine::input
{
    /// The service key ExposeToScript binds and the scripting facade resolves.
    // kInputScriptService now lives in foundation.input (:runtime) so per-instance owners (GameInstance)
    // can install their own runtime under it without importing this subsystem.

    /// How game-UI input routing treats the active source when it carries NO scene
    /// binding (see SetSourceProvider). The PLAYER's shell source owns the whole
    /// window, so un-bound input reaches every scene's UI (AllScenes - the default).
    /// An EDITOR-embedded runtime sets ScreenTierOnly: un-bound input (raw shell
    /// keystrokes before the Game tab exists, or the Game viewport's source while
    /// NOT playing) must never reach scene-tier game canvases in open editing pages -
    /// their HUDs stay VISIBLE (WYSIWYG) but deliberately not interactive.
    enum class UnboundInputScenePolicy : u8
    {
        AllScenes = 0,  // un-bound input reaches every scene's UI (player default)
        ScreenTierOnly, // un-bound input reaches only the scene-less screen tier (editor)
    };

    class InputSubsystem final : public foundation::runtime::Subsystem
    {
    public:
        /// `input` = the shell's device hub (null tolerated: headless runs read released).
        explicit InputSubsystem(foundation::shell::IInputManager* input) : m_shellSource(input) {}

        [[nodiscard]] ActionRuntime& Runtime() noexcept { return m_runtime; }

        /// This subsystem's runtime, then each run's (the player's, the editor's Game tabs': the
        /// ActionRuntime a run holds as a service, Documentation/Specs/run-context.md): the UI marks
        /// what it takes on every one (else a tap on a menu also reached the game's actions) and
        /// shows their touch controls. No runs here: this subsystem's runtime alone.
        using ActionRuntimeVisitor = Function<void(ActionRuntime&)>;
        void ForEachActionRuntime(const ActionRuntimeVisitor& visit)
        {
            visit(m_runtime);
            foundation::run::IRunManager* runs = Manager() != nullptr ? Manager()->RunManager() : nullptr;
            if (runs != nullptr)
            {
                runs->ForEachRun(
                    [&visit](foundation::run::IRun& run)
                    {
                        if (ActionRuntime* runtime = run.Get<ActionRuntime>())
                        {
                            visit(*runtime);
                        }
                    });
            }
        }

        /// The raw shell devices as an input source - a host points a GameInstance's per-instance input
        /// runtime here when there is no gated viewport (the standalone player reads the window directly).
        [[nodiscard]] IInputSourceProvider& ShellSource() noexcept { return m_shellSource; }

        /// Installs (copies) a map - from the cooked resource, a test, or hand-authored.
        void SetMap(const InputMap& map) { m_runtime.SetMap(map); }

        /// Overrides the device source (play-in-editor: the Game viewport's gated facades).
        /// Null restores the shell devices.
        ///
        /// `boundSceneKey` is the PER-SURFACE SCENE BINDING: the opaque
        /// identity of the scene this source represents - a Scene* used only for
        /// comparison, the render layer's SceneOverlayView::sceneKey convention, which
        /// keeps this module scene-agnostic. When bound, the UI pump routes pointer
        /// probing, keyboard/text, and gamepad navigation ONLY to that scene's UI root
        /// (plus the scene-less screen tier, which is modal while occupied) and publishes
        /// the consumption mask from that root alone - two interactive scenes visible at
        /// once can no longer cross-route on overlapping coordinates. Null = un-bound:
        /// UnboundScenePolicy() decides. The binding rides the override - clearing the
        /// provider clears it.
        void SetSourceProvider(IInputSourceProvider* provider,
                               const void* boundSceneKey = nullptr) noexcept
        {
            m_override = provider;
            m_boundSceneKey = (provider != nullptr) ? boundSceneKey : nullptr;
        }

        /// Clear the override IFF it currently points at `source` - so a source about to be destroyed
        /// (a closing editor Game tab's viewport source) can't leave `m_override` dangling for the next
        /// PumpInput/Update. Guarded so it never clears a DIFFERENT still-open tab's active source.
        void ClearSourceProviderIf(const IInputSourceProvider* source) noexcept
        {
            if (m_override == source)
            {
                m_override = nullptr;
                m_boundSceneKey = nullptr;
            }
        }

        /// The active source's scene binding (null = un-bound; see SetSourceProvider).
        [[nodiscard]] const void* BoundSceneKey() const noexcept { return m_boundSceneKey; }

        /// Routing for un-bound sources (see UnboundInputScenePolicy). The editor's
        /// embedded runtime sets ScreenTierOnly once at startup; the player keeps the
        /// AllScenes default.
        void SetUnboundScenePolicy(UnboundInputScenePolicy policy) noexcept
        {
            m_unboundScenePolicy = policy;
        }
        [[nodiscard]] UnboundInputScenePolicy UnboundScenePolicy() const noexcept
        {
            return m_unboundScenePolicy;
        }

        /// The device source actions currently evaluate against - the UI subsystem reads
        /// the SAME facades (so game UI sees viewport-transformed coordinates in the Game
        /// tab and window coordinates in the player, transparently).
        [[nodiscard]] IInputSourceProvider& ActiveSource() noexcept
        {
            return (m_override != nullptr) ? *m_override
                                           : static_cast<IInputSourceProvider&>(m_shellSource);
        }

        /// Binds THIS subsystem's runtime as `context`'s input service - the scripting
        /// facade (class Input below) resolves it per context, so two contexts can read
        /// two different runtimes (players; editor vs game). Call once per created context.
        void ExposeToScript(foundation::script::IScriptContext& scriptContext)
        {
            scriptContext.SetService(kInputScriptService, &m_runtime);
        }

        void Update(f32 deltaTime) override
        {
            if (foundation::runtime::SubsystemManager* context = Manager())
            {
                m_runtime.SetTimeScale(context->TimeScale());
            }
            IInputSourceProvider& devices = (m_override != nullptr)
                                                ? *m_override
                                                : static_cast<IInputSourceProvider&>(m_shellSource);
            m_runtime.Update(devices, deltaTime);
        }

    private:
        ShellInputSource m_shellSource;
        IInputSourceProvider* m_override = nullptr; // borrowed
        const void* m_boundSceneKey = nullptr; // the override's scene binding (comparison only)
        UnboundInputScenePolicy m_unboundScenePolicy = UnboundInputScenePolicy::AllScenes;
        ActionRuntime m_runtime;
    };

    // The scripting facade: a foreign class named `Input` whose STATIC methods resolve the
    // CURRENT script context's bound runtime (foundation.script's CurrentScriptContext seam,
    // pushed by the backend around every reflected dispatch). NO process globals: a context
    // without the service - or a call from outside any script - reads released. Some
    // backends cannot inject host objects as module globals, which is why the API is
    // statics-on-a-foreign-class rather than a passed object.
    class Input final : public Object
    {
        RTTI_OBJECT(Input, Object)
    public:
        [[nodiscard]] static ActionRuntime* Resolve()
        {
            foundation::script::IScriptContext* scriptContext = foundation::script::CurrentScriptContext();
            return scriptContext != nullptr
                       ? static_cast<ActionRuntime*>(scriptContext->GetService(kInputScriptService))
                       : nullptr;
        }

        [[nodiscard]] static bool isDown(String name)
        {
            ActionRuntime* rt = Resolve();
            return rt != nullptr && rt->IsDown(rt->Resolve(name.AsView()));
        }
        [[nodiscard]] static bool wasPressed(String name)
        {
            ActionRuntime* rt = Resolve();
            return rt != nullptr && rt->WasPressed(rt->Resolve(name.AsView()));
        }
        [[nodiscard]] static bool wasReleased(String name)
        {
            ActionRuntime* rt = Resolve();
            return rt != nullptr && rt->WasReleased(rt->Resolve(name.AsView()));
        }
        [[nodiscard]] static f32 value(String name)
        {
            ActionRuntime* rt = Resolve();
            return rt != nullptr ? rt->Value(rt->Resolve(name.AsView())) : 0.0f;
        }
        [[nodiscard]] static f32 valueX(String name) { return value(static_cast<String&&>(name)); }
        [[nodiscard]] static f32 valueY(String name)
        {
            ActionRuntime* rt = Resolve();
            return rt != nullptr ? rt->Value2D(rt->Resolve(name.AsView())).y : 0.0f;
        }
        /// A 2D action's value as one Float2 (Sedulous's Value2D): valueX and valueY together.
        [[nodiscard]] static Float2 value2D(String name)
        {
            ActionRuntime* rt = Resolve();
            return rt != nullptr ? rt->Value2D(rt->Resolve(name.AsView())) : Float2{0.0f, 0.0f};
        }
        static void pushSet(String name)
        {
            if (ActionRuntime* rt = Resolve())
            {
                rt->PushExclusiveSet(name.AsView());
            }
        }
        static void popSet()
        {
            if (ActionRuntime* rt = Resolve())
            {
                rt->PopExclusiveSet();
            }
        }
        /// Runs gamepad 0's motors for `seconds`: `low` the heavy, low-frequency motor and `high`
        /// the light, high-frequency one, each 0 to 1 (a crash 0.8, 0.4, 0.25; a footstep 0, 0.2,
        /// 0.05). It reaches the calling run's own pad (an editor Game tab's, the player's).
        static void rumble(f32 low, f32 high, f32 seconds) { rumble(0, low, high, seconds); }
        /// The same, for pad `gamepad` (0 first).
        static void rumble(i32 gamepad, f32 low, f32 high, f32 seconds)
        {
            if (ActionRuntime* rt = Resolve())
            {
                rt->Rumble(gamepad, low, high, seconds);
            }
        }
        /// Stops every pad's rumble (a run's end stops it too).
        static void stopRumble()
        {
            if (ActionRuntime* rt = Resolve())
            {
                rt->StopRumble();
            }
        }
        static void enableSet(String name, bool enabled)
        {
            if (ActionRuntime* rt = Resolve())
            {
                rt->EnableSet(name.AsView(), enabled);
            }
        }
    };

    /// Registers the facade into the global type registry (RegisterReflectedTypes then
    /// sweeps it into any script manager).
    void RegisterInputScriptFacade();
}

// Input::StaticType() reflection body + RegisterInputScriptFacade() live in
// InputSubsystemImpl.cpp (kept out of this interface; see gcc-module-interface-hygiene).

export namespace engine::input
{
    /// This domain's declaration (engine-composition.md D4): what it brings to a scene, to
    /// reflection, to the script surface and which resource modules come with it. Defined in the
    /// implementation unit (one instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& InputDomain() noexcept;
}
