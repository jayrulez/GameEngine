/// Engine::Scene - `engine.scene`.
///
/// The Context-level scene driver (game-instance.md §11 final): it owns the pure scene registry
/// (foundation.scene:composition's `SceneRegistry`) and drives every registered manager's per-frame
/// update + fixed update (UpdateOrder -500, so scenes tick before rendering reads them). It owns NO
/// scenes itself - there is no implicit "default" group. Every owner of scenes (a GameInstance, an
/// editor page) creates its OWN SceneManager and registers it here. The scene lifecycle + tick logic
/// live in SceneManager; this class only fans the Context time-scale / fixed-step to the managers
/// (foundation.scene stays runtime-free - the manager takes those as params).
///
/// Scene ASSEMBLY is declarative: each registered manager gets an installer that assembles new scenes
/// from the registry's `SceneComposition` (the single source of truth), then fans the `SystemsReady`
/// observer stage; teardown fans `Destroying`. Reactive wiring (cross-subsystem per-scene state) is
/// delivered through the observer stages - there is no other injection path.
///
/// Scratch / headless consumers that never tick on the Context lane (the editor's export transcode /
/// reachability scan) build a Scene directly via `Engine.SceneSurface::FullSceneComposition()`, not this
/// subsystem.

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

        // ---- observer broker (reactive scene-lifecycle stages) ----

        void RegisterObserver(ISceneObserver* observer, SceneLifecycleStage stage)
        {
            m_scenes.AddObserver(observer, stage);
        }
        void UnregisterObserver(ISceneObserver* observer) { m_scenes.RemoveObserver(observer); }

        // ---- composition (the declarative single source of truth) ----

        /// Install the declarative assignment of modules every registered manager's CreateScene assembles
        /// from. A manager registered BEFORE SetComposition reads the LIVE composition at create time, so
        /// ordering is irrelevant; scenes created before the composition was set are not retroactively
        /// rebuilt.
        void SetComposition(SceneComposition composition) { m_scenes.SetComposition(Move(composition)); }
        [[nodiscard]] SceneComposition& Composition() noexcept { return m_scenes.Composition(); }
        [[nodiscard]] const SceneComposition& Composition() const noexcept
        {
            return m_scenes.Composition();
        }

        // ---- manager registry (delegated to the pure SceneRegistry) ----

        /// Register a SceneManager (borrowed) so it ticks on the Context-driven lane. Wires the manager's
        /// install/uninstall hooks: CreateScene assembles from the live composition then fans SystemsReady;
        /// destroy/clear fans Destroying. The subsystem holds a `SceneManager*` (scene-lib type) - it never
        /// learns about its OWNER (GameInstance / editor page), so the dependency stays down. The owner
        /// unregisters before destroying the manager.
        void RegisterManager(SceneManager* manager)
        {
            if (manager == nullptr)
            {
                return;
            }
            m_scenes.RegisterManager(manager); // dedups; adds to the tick list

            // The installer closes over this subsystem: read the LIVE composition at create time. The
            // subsystem outlives every registered manager (the owner unregisters + clears its scenes before
            // subsystem teardown), so the `this` capture is safe under the same borrowed-pointer discipline
            // the manager list already relies on.
            SceneManager::SceneInstaller installer{
                [this](Scene& scene)
                {
                    // Composing fires BEFORE assembly (the stage's documented meaning - a
                    // pre-install hook; it was declared but never fired until the 2026-08-19
                    // review), SystemsReady after every module installed.
                    m_scenes.Notify(SceneLifecycleStage::Composing, scene);
                    m_scenes.Composition().Instantiate(scene);
                    m_scenes.Notify(SceneLifecycleStage::SystemsReady, scene);
                }};
            manager->SetSceneInstaller(Move(installer));

            SceneManager::SceneUninstaller uninstaller{
                [this](Scene& scene) { m_scenes.Notify(SceneLifecycleStage::Destroying, scene); }};
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
        SceneRegistry m_scenes; // pure manager/observer/sweep state (foundation.scene:composition)
    };

} // namespace engine::scene