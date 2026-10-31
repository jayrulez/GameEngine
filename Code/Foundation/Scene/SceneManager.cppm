// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Scene - the `:manager` partition.
///
/// SceneManager: a GROUP of scenes as a first-class scene-lib object.
/// It owns its scenes + a current scene + the group's time scale, ticks its own group's variable
/// + fixed lanes, and assembles/tears down each scene through type-erased install/uninstall hooks the
/// driver (SceneSubsystem) wires: assembly instantiates from a declarative composition, teardown notifies
/// observers. SceneManager itself stays composition- and runtime-agnostic.
///
/// The point of the abstraction (the linchpin of the GameInstance model): a `GameInstance` (runtime
/// layer) OWNS a SceneManager, so the dependency points DOWN - the scene lib never learns about the
/// runtime layer. A SceneManager reads only its OWN group config; it never reaches up to an instance.
/// Context-agnostic: the driver passes the context time-scale + fixed-step in (no runtime dependency,
/// so foundation.scene stays runtime-free).

module;
#include "Core/Prelude.h"

export module foundation.scene:manager;

import foundation.core;
import foundation.messaging; // EventBus (the scope bus injected into every created scene)
import foundation.run;       // IRun: the run every created scene belongs to
import :scene;
import :frame_time;

using namespace foundation::core;
namespace messaging = foundation::messaging;

export namespace foundation::scene
{

    class SceneManager
    {
    public:
        /// Type-erased scene assembler (the composition path). Invoked on CreateScene to build the
        /// scene's per-scene systems. Type-erased so :manager does not import :composition (that would
        /// be a partition cycle: :composition imports :manager). The caller owns the composition the
        /// installer closes over.
        using SceneInstaller = Function<void(Scene&)>;
        /// Type-erased scene teardown (the observer path's Destroying stage). Invoked on destroy/clear.
        using SceneUninstaller = Function<void(Scene&)>;

        // The allocator (required - the owner decides) backs every scene this
        // manager creates (and becomes each scene's own authority).
        explicit SceneManager(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        /// Install a composition installer: CreateScene assembles via `installer(scene)` while set (the
        /// only assembly path - there is no legacy fallback).
        void SetSceneInstaller(SceneInstaller installer) { m_installer = Move(installer); }
        void ClearSceneInstaller() { m_installer.Reset(); }
        /// Install a teardown hook: DestroyScene/Clear notify via `uninstaller(scene)` while set.
        void SetSceneUninstaller(SceneUninstaller uninstaller) { m_uninstaller = Move(uninstaller); }
        void ClearSceneUninstaller() { m_uninstaller.Reset(); }

        /// The scope bus every scene this manager creates BORROWS: a GameInstance sets
        /// its run bus here so `scene.events` and the run bus are one object. Applied before assembly, so
        /// the script systems bind to it. Null (default) leaves scenes with no bus.
        void SetSceneEventBus(messaging::EventBus* bus) noexcept { m_sceneEventBus = bus; }
        // The run every created scene belongs to (Scene::SetRun), set by the owning run.
        void SetSceneRun(foundation::run::IRun* run) noexcept { m_sceneRun = run; }

        /// The GROUP time scale - the `instance` term of dt = host x context x GROUP x scene. Default
        /// 1.0, so the editor / default manager applies no group scaling.
        /// Clamped to >= 0 like every other term of the chain (a negative dt would run time backward).
        void SetTimeScale(f32 scale) noexcept { m_timeScale = Max(scale, 0.0f); }
        [[nodiscard]] f32 TimeScale() const noexcept { return m_timeScale; }

        /// The current scene of the group (the multi-scene model) - what a game's script time-pairs
        /// with and what a scene transition repoints. First created scene becomes current by default.
        void SetCurrentScene(Scene* scene) noexcept { m_current = scene; }
        [[nodiscard]] Scene* CurrentScene() const noexcept { return m_current; }

        // ---- scene lifecycle ----

        // Create a scene. `activate` (default true = the classic behavior) adds it to the active
        // set (ticked + rendered) and makes it current if none is; when false the scene is OWNED
        // but INACTIVE - not ticked, not rendered, not the spawn target - until ActivateScene().
        // The async level load creates the scene inactive and activates it only once its resources
        // have finalized, so a half-resolved scene never ticks or renders (task #123).
        Scene* CreateScene(StringView name = u8"Scene", bool activate = true)
        {
            UniquePtr<Scene> owned = MakeUnique<Scene>(*m_allocator, *m_allocator, name);
            Scene* scene = owned.Get();
            m_scenes.PushBack(Move(owned));
            if (activate)
            {
                m_active.PushBack(scene);
                if (m_current == nullptr)
                {
                    m_current = scene;
                }
            }
            // Inject the scope's shared bus (no owned fallback)
            // BEFORE assembly, so systems binding at OnSceneCreate see the SAME bus emits
            // land on (a GameInstance sets this to its run bus; the editor page sets its
            // page bus). Null = a scope-less scratch scene: Events() stays null, nothing
            // emits, script handles no-op safely.
            scene->SetEventBus(m_sceneEventBus);
            scene->SetRun(m_sceneRun); // before assembly, so systems see their run at create
            // Scene assembly happens regardless of active, so LoadScene can populate an inactive scene
            // before it is activated.
            if (m_installer)
            {
                m_installer(*scene);
            }
            return scene;
        }

        // Add an owned-but-inactive scene to the active set (ticked + rendered); becomes current if
        // none is. No-op if already active or not owned by this manager.
        void ActivateScene(Scene* scene)
        {
            if (scene == nullptr || !Owns(scene) || IsActive(scene))
            {
                return;
            }
            m_active.PushBack(scene);
            if (m_current == nullptr)
            {
                m_current = scene;
            }
        }

        // Remove a scene from the active set (stops ticking + rendering) WITHOUT destroying it;
        // clears current if it was current. No-op if not active.
        void DeactivateScene(Scene* scene)
        {
            if (m_current == scene)
            {
                m_current = nullptr;
            }
            RemoveFromActive(scene);
        }

        [[nodiscard]] bool IsActive(const Scene* scene) const noexcept
        {
            for (const Scene* s : m_active)
            {
                if (s == scene)
                {
                    return true;
                }
            }
            return false;
        }
        // Deferred to the end of Update if called while updating.
        void DestroyScene(Scene* scene)
        {
            if (scene == nullptr)
            {
                return;
            }
            if (m_updating)
            {
                m_pendingRemove.PushBack(scene);
                return;
            }
            DestroyImmediate(scene);
        }
        [[nodiscard]] Scene* GetScene(StringView name)
        {
            for (Scene* s : m_active)
            {
                if (s->Name() == name)
                {
                    return s;
                }
            }
            return nullptr;
        }
        [[nodiscard]] Span<Scene* const> ActiveScenes() const noexcept
        {
            return {m_active.Data(), m_active.Size()};
        }
        [[nodiscard]] usize SceneCount() const noexcept { return m_scenes.Size(); }

        template <typename Fn>
        void ForEachScene(Fn&& fn)
        {
            for (auto& scene : m_scenes)
            {
                fn(*scene);
            }
        }

        // ---- ticking (Context-agnostic - the driver passes the context factors) ----

        // Fixed lane. `rawHostDt` = un-context-scaled dt; applies context x group x scene. `contextStep`
        // sets each scene's fixed timing (the fixed accumulator itself is per-scene, already).
        // Fixed lane: seed each active scene's stepper from the lane's configured step, then
        // advance per-scene fixed time on the FULL chain. `time` carries host x context (built
        // by the driver); THIS manager contributes the group term, each scene its own term -
        // the one place the chain is composed (FrameTime).
        void BeginFrame(const FrameTime& time)
        {
            for (Scene* s : m_active)
            {
                if (time.fixedStep > 0.0f && s->FixedTimeStep() != time.fixedStep)
                {
                    s->SetFixedTiming(time.fixedStep, 4);
                }
                (void)s->AdvanceTime(time.ContextDt() * m_timeScale * s->TimeScale());
            }
        }
        // Variable lane: same chain, same single composition point.
        void Update(const FrameTime& time)
        {
            m_updating = true;
            for (Scene* s : m_active)
            {
                s->Update(time.ContextDt() * m_timeScale * s->TimeScale());
            }
            m_updating = false;
            ProcessPendingRemoves();
        }

        // Destroy every scene in the group (notifying) - the group teardown.
        void Clear()
        {
            for (usize i = m_scenes.Size(); i-- > 0;)
            {
                NotifyDestroyed(*m_scenes[i]);
            }
            m_active.Clear();
            m_pendingRemove.Clear();
            m_current = nullptr;
            m_scenes.Clear(); // UniquePtr frees each Scene
        }

    private:
        void NotifyDestroyed(Scene& scene)
        {
            if (m_uninstaller)
            {
                m_uninstaller(scene);
            }
        }

        void DestroyImmediate(Scene* scene)
        {
            // A running scene stops before it goes: its systems' stop hooks are what unhook them
            // from whatever outlives the scene (a script system's handlers on a run's event bus,
            // a Level's onStop). A no-op for a scene that is not running.
            scene->Stop();
            NotifyDestroyed(*scene);
            if (m_current == scene)
            {
                m_current = nullptr;
            }
            RemoveFromActive(scene);
            for (usize i = 0; i < m_scenes.Size(); ++i)
            {
                if (m_scenes[i].Get() == scene)
                {
                    m_scenes.RemoveAt(i);
                    break;
                } // frees the Scene
            }
        }

        // Drop `scene` from the active set (no destroy). Shared by DeactivateScene + DestroyImmediate.
        void RemoveFromActive(Scene* scene)
        {
            for (usize i = 0; i < m_active.Size(); ++i)
            {
                if (m_active[i] == scene)
                {
                    m_active.RemoveAt(i);
                    return;
                }
            }
        }

        [[nodiscard]] bool Owns(const Scene* scene) const noexcept
        {
            for (const auto& owned : m_scenes)
            {
                if (owned.Get() == scene)
                {
                    return true;
                }
            }
            return false;
        }
        void ProcessPendingRemoves()
        {
            for (Scene* s : m_pendingRemove)
            {
                DestroyImmediate(s);
            }
            m_pendingRemove.Clear();
        }

        SceneInstaller m_installer;   // composition path (type-erased; owned, callable or empty)
        SceneUninstaller m_uninstaller; // teardown path (type-erased; owned, callable or empty)
        messaging::EventBus* m_sceneEventBus = nullptr; // borrowed scope bus for created scenes
        foundation::run::IRun* m_sceneRun = nullptr;    // the run created scenes belong to
        f32 m_timeScale = 1.0f;       // the group / instance term
        Scene* m_current = nullptr;   // the group's current scene
        IAllocator* m_allocator;
        Array<UniquePtr<Scene>> m_scenes; // ownership
        Array<Scene*> m_active;       // active (non-owning)
        Array<Scene*> m_pendingRemove;
        bool m_updating = false;
    };

} // namespace foundation::scene