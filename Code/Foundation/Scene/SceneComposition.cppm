/// Foundation::Scene - the `:composition` partition.
///
/// The declarative assembly layer (scene-composition.md): separates "what systems a scene is made of"
/// (a static blueprint, `SceneModule` + `SceneComposition`) from "who reacts to a scene's lifecycle"
/// (`ISceneObserver` + `SceneLifecycleStage`), and centralizes the per-scene time-scale chain in
/// `FrameTime`. `SceneRegistry` is the pure, non-Subsystem state that a future thin `SceneSubsystem`
/// will drive - the app-wide observer list + the registered `SceneManager` list + the cross-scene
/// sweeps, testable with no `Context`.
///
/// All types are runtime-free (foundation.scene never reaches up to the runtime layer) and are built
/// on `foundation.core` value types only. `SceneModule` and `SceneComposition` are the single source of
/// truth that both the runtime path and headless scene consumers (Engine.SceneSurface transcode / MCP
/// validate) instantiate scenes from, replacing the imperative, duplicated `AddAllSceneManagers` list
/// and its `kSceneSystemCount` count tripwire.

module;
#include "Core/Prelude.h"

export module foundation.scene:composition;

import foundation.core;
import :scene;   // Scene (the observers/composition reference it by value)
import :manager; // SceneManager (SceneRegistry drives managers on the lanes)

using namespace foundation::core;

export namespace foundation::scene
{

    // ---- FrameTime: the time-scale chain (host x context x group x scene) --------------------------
    // One value type carrying the raw host delta and every scale multiplier a scene lane needs, so a
    // caller computes the effective dt in ONE place instead of hand-multiplying in SceneManager,
    // GameInstance::TickScript, and the input/net drives (the drift scene-composition.md §strains #4
    // identifies). The fixed-step accumulator itself stays per-scene (each Scene owns its FixedStepper);
    // `fixedStep` here is just the lane's configured step, published alongside the scales.
    struct FrameTime
    {
        f32 rawDt = 0.0f;         // host dt, unscaled
        f32 contextScale = 1.0f;  // app-wide term
        f32 groupScale = 1.0f;    // the group / instance term
        f32 sceneScale = 1.0f;    // the per-scene term
        f32 fixedStep = 1.0f / 60.0f;

        FrameTime() = default;
        FrameTime(f32 raw, f32 context = 1.0f, f32 group = 1.0f, f32 scene = 1.0f,
                  f32 step = 1.0f / 60.0f) noexcept
            : rawDt(raw), contextScale(context), groupScale(group), sceneScale(scene), fixedStep(step)
        {
        }

        // The dt a context-level subsystem sees for the variable lane (host x context).
        [[nodiscard]] f32 ContextDt() const noexcept { return rawDt * contextScale; }

        // The dt a scene's variable lane sees (host x context x group x scene).
        [[nodiscard]] f32 SceneDt() const noexcept
        {
            return rawDt * contextScale * groupScale * sceneScale;
        }
    };

    // ---- Lifecycle stage + observer -----------------------------------------------------------------
    // The explicit, ordered lifecycle a scene walks. Assembly no longer happens through observation:
    // a module installs systems (SceneComposition), and an observer reacts at one of these stages.
    enum class SceneLifecycleStage : u8
    {
        Composing = 0, // a scene object exists; systems are being installed into it
        SystemsReady,  // all modules installed their systems; cross-system state is now reachable
        Started,       // the scene entered play/simulation
        Stopped,       // the scene left play/simulation
        Destroying,    // the scene is being torn down (drop references here)
        Count,
    };

    class Scene; // defined in :scene (same module)

    // An observer reacts to ONE stage of a scene's lifecycle. Register it on a SceneRegistry with
    // AddObserver(observer, stage). Order() (lower first) breaks ties within a stage - the replacement
    // the explicit, data-driven replacement for the old injection flow.
    class ISceneObserver
    {
    public:
        virtual ~ISceneObserver() = default;

        virtual void OnComposing(Scene& /*scene*/) {}
        virtual void OnSystemsReady(Scene& /*scene*/) {}
        virtual void OnStarted(Scene& /*scene*/) {}
        virtual void OnStopped(Scene& /*scene*/) {}
        virtual void OnDestroying(Scene& /*scene*/) {}

        [[nodiscard]] virtual i32 Order() const noexcept { return 0; }
    };

    // ---- SceneModule: one domain's declarative "how to build a scene" -------------------------------
    // Replaces the free Add<Domain>SceneManagers + Register<Domain>ComponentReflection pair. `dependsOn`
    // (pointers to sibling modules) declares cross-domain construction order as data, not tick order.
    class SceneModule
    {
    public:
        using InstallFn = void (*)(Scene&);          // constructs managers / settings systems
        using RegisterReflectionFn = void (*)();     // registers component reflection (idempotent)

        SceneModule() = default;
        SceneModule(StringView id, InstallFn install, RegisterReflectionFn registerReflection,
                    Span<const SceneModule*> dependsOn = {})
            : m_id(id), m_install(install), m_registerReflection(registerReflection),
              m_dependsOn(dependsOn)
        {
        }

        [[nodiscard]] StringView Id() const noexcept { return m_id; }
        [[nodiscard]] Span<const SceneModule*> DependsOn() const noexcept { return m_dependsOn; }
        [[nodiscard]] bool HasInstall() const noexcept { return m_install != nullptr; }

        void Install(Scene& scene) const
        {
            if (m_install != nullptr)
            {
                m_install(scene);
            }
        }
        void RegisterReflection() const
        {
            if (m_registerReflection != nullptr)
            {
                m_registerReflection();
            }
        }

    private:
        StringView m_id;
        InstallFn m_install = nullptr;
        RegisterReflectionFn m_registerReflection = nullptr;
        Span<const SceneModule*> m_dependsOn;
    };

    // ---- SceneComposition: the one-time-built, topological blueprint --------------------------------
    class SceneComposition
    {
    public:
        SceneComposition() = default;

        // Builds the topologically sorted module order from `modules`. A module whose dependency is
        // absent from `modules` treats that dependency as satisfied (it is out of scope for this
        // composition). A dependency cycle is broken by emitting the first remaining module, so Build
        // always terminates and contains every module exactly once.
        static SceneComposition Build(Span<const SceneModule*> modules)
        {
            SceneComposition comp;

            auto inSet = [&](const SceneModule* m) noexcept
            {
                for (const SceneModule* x : modules)
                {
                    if (x == m)
                    {
                        return true;
                    }
                }
                return false;
            };
            auto placed = [&](const SceneModule* m) noexcept
            {
                for (const SceneModule* x : comp.m_order)
                {
                    if (x == m)
                    {
                        return true;
                    }
                }
                return false;
            };

            Array<const SceneModule*> remaining;
            for (const SceneModule* m : modules)
            {
                remaining.PushBack(m);
            }

            while (!remaining.IsEmpty())
            {
                usize pick = remaining.Size(); // sentinel: none ready
                for (usize i = 0; i < remaining.Size(); ++i)
                {
                    const SceneModule* m = remaining[i];
                    bool ready = true;
                    for (const SceneModule* dep : m->DependsOn())
                    {
                        if (inSet(dep) && !placed(dep))
                        {
                            ready = false;
                            break;
                        }
                    }
                    if (ready)
                    {
                        pick = i;
                        break;
                    }
                }
                if (pick == remaining.Size())
                {
                    pick = 0; // cycle: make progress by emitting the first remaining module
                }
                comp.m_order.PushBack(remaining[pick]);
                remaining.RemoveAt(pick);
            }
            return comp;
        }

        // Adds every module's declared systems to `scene`, in dependency order.
        void Instantiate(Scene& scene) const
        {
            for (const SceneModule* m : m_order)
            {
                m->Install(scene);
            }
        }

        // Registers every module's component reflection (idempotent aggregate; call once at startup).
        void RegisterReflection() const
        {
            for (const SceneModule* m : m_order)
            {
                m->RegisterReflection();
            }
        }

        [[nodiscard]] usize ModuleCount() const noexcept { return m_order.Size(); }

    private:
        Array<const SceneModule*> m_order; // topologically sorted
    };

    // ---- SceneRegistry: the pure scene-hardware state (testable with no Context) --------------------
    // Holds the composition + observer list + the registered SceneManager list + the cross-scene
    // sweeps. A future thin SceneSubsystem will own one and just fan the Context lanes over it.
    class SceneRegistry
    {
    public:
        SceneRegistry() = default;

        // ---- composition ----
        void SetComposition(SceneComposition composition) { m_composition = Move(composition); }
        [[nodiscard]] SceneComposition& Composition() noexcept { return m_composition; }
        [[nodiscard]] const SceneComposition& Composition() const noexcept { return m_composition; }
        void RegisterReflection() const { m_composition.RegisterReflection(); }

        // ---- observers (react to the scene lifecycle; one stage per registration) ----
        void AddObserver(ISceneObserver* observer, SceneLifecycleStage stage)
        {
            if (observer == nullptr)
            {
                return;
            }
            for (const ObserverEntry& e : m_observers)
            {
                if (e.observer == observer && e.stage == stage)
                {
                    return; // idempotent
                }
            }
            m_observers.PushBack(ObserverEntry{observer, stage});
            // Stable insertion-sort by Order() (lower first), so stage peers run in declared order.
            usize i = m_observers.Size() - 1;
            while (i > 0 && m_observers[i - 1].observer->Order() > observer->Order())
            {
                const ObserverEntry prev = m_observers[i - 1];
                m_observers[i - 1] = m_observers[i];
                m_observers[i] = prev;
                --i;
            }
        }

        // Removes every registration for `observer` (all stages).
        void RemoveObserver(ISceneObserver* observer)
        {
            for (usize i = m_observers.Size(); i-- > 0;)
            {
                if (m_observers[i].observer == observer)
                {
                    m_observers.RemoveAt(i);
                }
            }
        }

        // Fires `stage` to every observer registered for it, in Order() order.
        void Notify(SceneLifecycleStage stage, Scene& scene)
        {
            for (const ObserverEntry& e : m_observers)
            {
                if (e.stage != stage)
                {
                    continue;
                }
                switch (stage)
                {
                case SceneLifecycleStage::Composing: e.observer->OnComposing(scene); break;
                case SceneLifecycleStage::SystemsReady: e.observer->OnSystemsReady(scene); break;
                case SceneLifecycleStage::Started: e.observer->OnStarted(scene); break;
                case SceneLifecycleStage::Stopped: e.observer->OnStopped(scene); break;
                case SceneLifecycleStage::Destroying: e.observer->OnDestroying(scene); break;
                case SceneLifecycleStage::Count: break;
                }
            }
        }

        // ---- manager registry (every SceneManager ticks on the Context-driven lane) ----
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

        [[nodiscard]] usize ManagerCount() const noexcept { return m_managers.Size(); }

        template <typename Fn>
        void ForEachManager(Fn&& fn)
        {
            for (SceneManager* m : m_managers)
            {
                fn(*m);
            }
        }

        // A read-only registry sweep across every live scene in every manager (prefab rebuild, export).
        template <typename Fn>
        void ForEachScene(Fn&& fn)
        {
            for (SceneManager* m : m_managers)
            {
                m->ForEachScene([&fn](Scene& s) { fn(s); });
            }
        }

        // ---- lane drive (the fan-out a thin SceneSubsystem performs each frame) ----
        void BeginFrame(f32 rawDt, f32 contextScale, f32 contextStep)
        {
            for (SceneManager* m : m_managers)
            {
                m->BeginFrame(rawDt, contextScale, contextStep);
            }
        }
        void Update(f32 contextScaledDt)
        {
            for (SceneManager* m : m_managers)
            {
                m->Update(contextScaledDt);
            }
        }

    private:
        struct ObserverEntry
        {
            ISceneObserver* observer;
            SceneLifecycleStage stage;
        };

        SceneComposition m_composition;
        Array<SceneManager*> m_managers;       // borrowed; owned by pages / instances
        Array<ObserverEntry> m_observers;      // Order()-sorted within stage
    };

} // namespace foundation::scene