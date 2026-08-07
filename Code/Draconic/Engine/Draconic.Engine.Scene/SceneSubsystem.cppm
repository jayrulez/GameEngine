/// Draconic::SceneSubsystem - `draconic.engine.scene`.
///
/// The Context-level scene driver, a PURE REGISTRY (game-instance.md §11 final): it owns the app-wide
/// ISceneAware registry and a list of registered SceneManagers, and drives every registered manager's
/// per-frame update + fixed update (UpdateOrder -500, so scenes tick before rendering reads them). It
/// owns NO scenes itself - there is no implicit "default" group. Every owner of scenes (a GameInstance,
/// an editor page) creates its OWN SceneManager over the shared registry and registers it here. The
/// scene lifecycle + tick logic live in SceneManager; this class only fans the Context time-scale /
/// fixed-step to the managers (draconic.scene stays runtime-free - the manager takes those as params).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.engine.scene;

import draconic.foundation;
import draconic.runtime;
import draconic.scene;

using namespace draconic::foundation;

export namespace draconic::scene
{

    class SceneSubsystem final : public draconic::runtime::Subsystem
    {
    public:
        [[nodiscard]] i32 UpdateOrder() const noexcept override
        {
            return -500;
        } // scenes tick early

        // ---- ISceneAware broker (the registry is app-wide; every manager fans out through it) ----

        void RegisterSceneAware(ISceneAware* aware) { m_registry.Register(aware); }
        void UnregisterSceneAware(ISceneAware* aware) { m_registry.Unregister(aware); }

        /// The shared aware registry, so an owner can build its own SceneManager over the same app-wide
        /// aware list (game-instance.md §11): `SceneManager sm(&scenes->AwareRegistry());`.
        [[nodiscard]] SceneAwareRegistry& AwareRegistry() noexcept { return m_registry; }

        // ---- manager registry (every SceneManager registers to tick on the Context lane) ----

        /// Register a SceneManager (borrowed) so it ticks on the Context-driven lane. The subsystem holds
        /// a `SceneManager*` (scene-lib type) - it never learns about its OWNER (GameInstance / editor
        /// page), so the dependency stays down. The owner unregisters before destroying the manager.
        void RegisterManager(SceneManager* manager)
        {
            if (manager == nullptr)
            {
                return;
            }
            for (SceneManager* m : m_managers)
            {
                if (m == manager)
                {
                    return;
                }
            }
            m_managers.PushBack(manager);
        }
        void UnregisterManager(SceneManager* manager)
        {
            for (usize i = 0; i < m_managers.Size(); ++i)
            {
                if (m_managers[i] == manager)
                {
                    m_managers.RemoveAt(i);
                    return;
                }
            }
        }

        /// Visit every registered manager (tooling sweeps that must reach all live scenes across owners).
        template <typename Fn>
        void ForEachManager(Fn&& fn)
        {
            for (SceneManager* m : m_managers)
            {
                fn(*m);
            }
        }

        /// Visit every live scene across ALL registered managers - a read-only registry sweep (NOT scene
        /// ownership): prefab-instance rebuild after a template save, export scans. Each owner still owns
        /// its own scenes on its own manager.
        template <typename Fn>
        void ForEachScene(Fn&& fn)
        {
            for (SceneManager* m : m_managers)
            {
                m->ForEachScene([&fn](Scene& s) { fn(s); });
            }
        }

        // ---- subsystem frame phases (drive every registered manager; Context factors applied here) ----

        // Fixed stepping is PER SCENE (each scene owns a FixedStepper): BeginFrame runs it so fixed-rate
        // state (physics poses + alpha) is fresh BEFORE any subsystem's Update reads it. BeginFrame gets the
        // RAW host dt (context scale applied inside the manager); Update gets context-scaled dt.
        void BeginFrame(f32 deltaTime) override
        {
            const f32 contextScale = GetContext() != nullptr ? GetContext()->TimeScale() : 1.0f;
            const f32 contextStep = GetContext() != nullptr ? GetContext()->FixedTimeStep() : 0.0f;
            for (SceneManager* m : m_managers)
            {
                m->BeginFrame(deltaTime, contextScale, contextStep);
            }
        }
        void Update(f32 deltaTime) override
        {
            for (SceneManager* m : m_managers)
            {
                m->Update(deltaTime);
            }
        }
        // OnShutdown: nothing to clear - every manager is cleared by its owner.

    private:
        SceneAwareRegistry m_registry; // app-wide aware list (declared first)
        Array<SceneManager*>
            m_managers; // all registered managers (borrowed; owned by pages/instances)
    };

} // namespace draconic::scene
