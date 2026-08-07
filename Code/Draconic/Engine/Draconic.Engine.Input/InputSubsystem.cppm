// Draconic::InputSubsystem - the `draconic.engine.input` module.
//
// The runtime hookup: owns the ActionRuntime + the device provider and evaluates once per
// frame in Update (before scenes tick - Subsystem registration order puts input ahead of
// the scene subsystem in every assembly that adds it first). The provider seam is the
// play-in-editor story: the player passes the shell's devices, the editor's Game tab will
// pass its viewport's gated InputSurface facades.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.engine.input;

import draconic.foundation;
import draconic.shell;
import draconic.runtime;
import draconic.script;
import draconic.input;

using namespace draconic::foundation;

export namespace draconic::input
{
    /// The service key ExposeToScript binds and the scripting facade resolves.
    // kInputScriptService now lives in draconic.input (:runtime) so per-instance owners (GameInstance)
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

    class InputSubsystem final : public draconic::runtime::Subsystem
    {
    public:
        /// `input` = the shell's device hub (null tolerated: headless runs read released).
        explicit InputSubsystem(draconic::shell::IInputManager* input) : m_shellSource(input) {}

        [[nodiscard]] ActionRuntime& Runtime() noexcept { return m_runtime; }

        /// The raw shell devices as an input source - a host points a GameInstance's per-instance input
        /// runtime here when there is no gated viewport (the standalone player reads the window directly).
        [[nodiscard]] IInputSourceProvider& ShellSource() noexcept { return m_shellSource; }

        /// Installs (copies) a map - from the cooked resource, a test, or hand-authored.
        void SetMap(const InputMap& map) { m_runtime.SetMap(map); }

        /// Overrides the device source (play-in-editor: the Game viewport's gated facades).
        /// Null restores the shell devices.
        ///
        /// `boundSceneKey` is the PER-SURFACE SCENE BINDING (game-ui.md §9): the opaque
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
        void ExposeToScript(draconic::script::IScriptContext& context)
        {
            context.SetService(kInputScriptService, &m_runtime);
        }

        void Update(f32 deltaTime) override
        {
            if (draconic::runtime::Context* context = GetContext())
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
    // CURRENT script context's bound runtime (draconic.script's CurrentScriptContext seam,
    // pushed by the backend around every reflected dispatch). NO process globals: a context
    // without the service - or a call from outside any script - reads released. The Wren
    // backend cannot inject host objects as module globals (wren has no host-side variable
    // set), which is why the API is statics-on-a-foreign-class rather than a passed object.
    class Input final : public Object
    {
        DRACONIC_OBJECT(Input, Object)
    public:
        [[nodiscard]] static ActionRuntime* Resolve()
        {
            draconic::script::IScriptContext* context = draconic::script::CurrentScriptContext();
            return context != nullptr
                       ? static_cast<ActionRuntime*>(context->GetService(kInputScriptService))
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
