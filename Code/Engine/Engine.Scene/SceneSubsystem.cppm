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
/// Scene ASSEMBLY is pluggable: each registered manager gets a composition installer that assembles new
/// scenes from the registry's `SceneComposition` when one is configured, and falls back to the legacy
/// ISceneAware two-pass otherwise (default - the current behavior, unchanged). The manager list, the
/// cross-scene sweeps, the observer list, and the lane fan-out live in the pure `SceneRegistry`.

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

        // ---- ISceneAware broker (the registry is app-wide; every manager fans out through it) ----

        void RegisterSceneAware(ISceneAware* aware) { m_awareRegistry.Register(aware); }
        void UnregisterSceneAware(ISceneAware* aware) { m_awareRegistry.Unregister(aware); }

        /// The shared aware registry, so an owner can build its own SceneManager over the same app-wide
        /// aware list (game-instance.md §11): `SceneManager sm(&scenes->AwareRegistry());`.
        [[nodiscard]] SceneAwareRegistry& AwareRegistry() noexcept { return m_awareRegistry; }

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
        /// scene installer so its CreateScene assembles from the composition (fallback: legacy aware
        /// two-pass when no composition is configured). The subsystem holds a `SceneManager*` (scene-lib
        /// type) - it never learns about its OWNER (GameInstance / editor page), so the dependency stays
        /// down. The owner unregisters before destroying the manager.
        void RegisterManager(SceneManager* manager)
        {
            if (manager == nullptr)
            {
                return;
            }
            m_scenes.RegisterManager(manager); // dedups; adds to the tick list

            // The installer closes over this subsystem: read the LIVE composition at create time, with the
            // legacy aware registry as the fallback. The subsystem outlives every registered manager (the
            // owner unregisters + clears its scenes before subsystem teardown), so the `this` capture is
            // safe under the same borrowed-pointer discipline the manager list already relies on.
            SceneManager::SceneInstaller installer{
                [this](Scene& scene)
                {
                    if (m_scenes.Composition().ModuleCount() > 0)
                    {
                        m_scenes.Composition().Instantiate(scene);
                    }
                    else
                    {
                        m_awareRegistry.NotifyCreated(scene);
                    }
                }};
            manager->SetSceneInstaller(Move(installer));
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