/// Engine::Scene - `engine.scene`.
///
/// The Context-level scene driver (game-instance.md §11 final): it owns the app-wide ISceneAware
/// registry and the pure scene registry (foundation.scene:composition's `SceneRegistry`), and drives
/// every registered manager's per-frame update + fixed update (UpdateOrder -500, so scenes tick before
/// rendering reads them). It owns NO scenes itself - there is no implicit "default" group. Every owner
/// of scenes (a GameInstance, an editor page) creates its OWN SceneManager over the shared registry and
/// registers it here. The scene lifecycle + tick logic live in SceneManager; this class only fans the
/// Context time-scale / fixed-step to the managers (foundation.scene stays runtime-free - the manager
/// takes those as params).
///
/// Scene ASSEMBLY is pluggable: each registered manager gets an installer that assembles new scenes from
/// the registry's `SceneComposition` when one is configured, and otherwise falls back to the legacy
/// ISceneAware two-pass (pure assembly). Reactive wiring (cross-subsystem per-scene state) is delivered
/// through the observer stages (SystemsReady / Destroying) AFTER assembly in BOTH paths, so a scene built
/// either way is wired identically. Unregistered scratch managers (the editor's transcode/scan) keep the
/// legacy ISceneAware assembly-only path and never fan observers - inert scenes need no wiring.

module;
#include "Core/Prelude.h"

export module engine.scene;

import foundation.core;
import foundation.runtime;
import foundation.scene;

using namespace foundation::core;
using namespace foundation::scene;

export namespace engine::scene
{

    class SceneSubsystem final : public foundation::runtime::Subsystem
    {
    public:
        [[nodiscard]] i32 UpdateOrder() const noexcept override
        {
            return -500;
        } // scenes tick early

        // ---- ISceneAware broker (the registry is app-wide; the legacy assembly path) ----

        void RegisterSceneAware(ISceneAware* aware) { m_awareRegistry.Register(aware); }
        void UnregisterSceneAware(ISceneAware* aware) { m_awareRegistry.Unregister(aware); }

        /// The shared aware registry, so an owner can build its own SceneManager over the same app-wide
        /// aware list (game-instance.md §11): `SceneManager sm(&scenes->AwareRegistry());`.
        [[nodiscard]] SceneAwareRegistry& AwareRegistry() noexcept { return m_awareRegistry; }

        // ---- observer broker (the reactive scene-lifecycle stages; the composition path) ----

        void RegisterObserver(ISceneObserver* observer, SceneLifecycleStage stage)
        {
            m_scenes.AddObserver(observer, stage);
        }
        void UnregisterObserver(ISceneObserver* observer) { m_scenes.RemoveObserver(observer); }

        // ---- composition (the declarative single source of truth, empty until the app sets it) ----

        /// Install the declarative assignment of modules the app wants every scene built from. Once set,
        /// newly created scenes on every registered manager are assembled through it; scenes created
        /// BEFORE SetComposition used the legacy ISceneAware path (a composition is applied at
        /// CreateScene time, not retroactively).
        void SetComposition(SceneComposition composition) { m_scenes.SetComposition(Move(composition)); }
        [[nodiscard]] SceneComposition& Composition() noexcept { return m_scenes.Composition(); }
        [[nodiscard]] const SceneComposition& Composition() const noexcept
        {
            return m_scenes.Composition();
        }

        // ---- manager registry (delegated to the pure SceneRegistry) ----

        /// Register a SceneManager (borrowed) so it ticks on the Context-driven lane. Wires the manager's
        /// install/uninstall hooks so CreateScene assembles from the composition (fallback: legacy aware
        /// two-pass when no composition is configured) and ALWAYS fans the observer stages after assembly,
        /// while destroy/clear fans Destroying. The subsystem holds a `SceneManager*` (scene-lib type) -
        /// it never learns about its OWNER (GameInstance / editor page), so the dependency stays down.
        /// The owner unregisters before destroying the manager.
        void RegisterManager(SceneManager* manager)
        {
            if (manager == nullptr)
            {
                return;
            }
            m_scenes.RegisterManager(manager); // dedups; adds to the tick list

            // The closure reads the LIVE composition at create time, with the legacy aware registry as the
            // assembly fallback. Observer stages fire in BOTH paths (reactive wiring is source-agnostic).
            // The subsystem outlives every registered manager (the owner unregisters + clears its scenes
            // before subsystem teardown), so the `this` capture is safe under the same borrowed-pointer
            // discipline the manager list already relies on.
            SceneManager::SceneInstaller installer{
                [this](Scene& scene)
                {
                    // Assembly is composition XOR legacy: a configured composition already covers every
                    // built-in domain (Render/Physics/Audio/Script/Particles/UI/Navigation/Animation/Net),
                    // so firing the legacy ISceneAware two-pass too would double-add each manager.
                    // Custom/plugin subsystems adopt the composition by registering their own SceneModule.
                    if (m_scenes.Composition().ModuleCount() > 0)
                    {
                        m_scenes.Composition().Instantiate(scene);
                    }
                    else
                    {
                        m_awareRegistry.NotifyCreated(scene); // legacy two-pass (assembly + ready)
                    }
                    // Reactive wiring (observers) runs after assembly in BOTH paths.
                    m_scenes.Notify(SceneLifecycleStage::SystemsReady, scene);
                }};
            manager->SetSceneInstaller(Move(installer));

            SceneManager::SceneUninstaller uninstaller{
                [this](Scene& scene)
                {
                    // Reactive (migrated observers) + legacy (unmigrated ISceneAware) teardown. Migrated
                    // subsystems leave OnSceneDestroyed a no-op, so this never double-fires cleanup.
                    m_scenes.Notify(SceneLifecycleStage::Destroying, scene);
                    m_awareRegistry.NotifyDestroyed(scene);
                }};
            manager->SetSceneUninstaller(Move(uninstaller));
        }
        void UnregisterManager(SceneManager* manager) { m_scenes.UnregisterManager(manager); }

        /// The pure scene registry backing this subsystem (manager list + observer list + sweeps +
        /// lane fan-out); exposed so owners/tooling can reach the composition and observer state.
        [[nodiscard]] SceneRegistry& Registry() noexcept { return m_scenes; }

        /// Visit every registered manager (tooling sweeps that must reach all live scenes across owners).
        template <typename Fn>
        void ForEachManager(Fn&& fn)
        {
            m_scenes.ForEachManager(fn);
        }

        /// Visit every live scene across ALL registered managers - a read-only registry sweep (NOT scene
        /// ownership): prefab-instance rebuild after a template save, export scans. Each owner still owns
        /// its own scenes on its own manager.
        template <typename Fn>
        void ForEachScene(Fn&& fn)
        {
            m_scenes.ForEachScene(fn);
        }

        // ---- subsystem frame phases (drive every registered manager; Context factors applied here) ----

        // Fixed stepping is PER SCENE (each scene owns a FixedStepper): BeginFrame runs it so fixed-rate
        // state (physics poses + alpha) is fresh BEFORE any subsystem's Update reads it. BeginFrame gets the
        // RAW host dt (context scale applied inside the manager); Update gets context-scaled dt.
        void BeginFrame(f32 deltaTime) override
        {
            const f32 contextScale = GetContext() != nullptr ? GetContext()->TimeScale() : 1.0f;
            const f32 contextStep = GetContext() != nullptr ? GetContext()->FixedTimeStep() : 0.0f;
            m_scenes.BeginFrame(deltaTime, contextScale, contextStep);
        }
        void Update(f32 deltaTime) override
        {
            m_scenes.Update(deltaTime);
        }
        // OnShutdown: nothing to clear - every manager is cleared by its owner.

    private:
        SceneAwareRegistry m_awareRegistry; // app-wide aware list (the ISceneAware broker + fallback)
        SceneRegistry m_scenes;             // pure manager/observer/sweep state (foundation.scene:composition)
    };

} // namespace engine::scene