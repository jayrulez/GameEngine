/// Draconic::Scene - the `:manager` partition.
///
/// SceneManager: a GROUP of scenes as a first-class scene-lib object (docs/design/game-instance.md
/// §11). It owns its scenes + a current scene + the group's time scale, ticks its own group's variable
/// + fixed lanes, and fans out ISceneAware lifecycle through a shared SceneAwareRegistry.
///
/// The point of the abstraction (the linchpin of the GameInstance model): a `GameInstance` (runtime
/// layer) OWNS a SceneManager, so the dependency points DOWN - the scene lib never learns about the
/// runtime layer. A SceneManager reads only its OWN group config; it never reaches up to an instance.
/// SceneSubsystem owns the app-wide SceneAwareRegistry + a DEFAULT SceneManager (loose / editor scenes);
/// instances own their own. Context-agnostic: the driver passes the context time-scale + fixed-step in
/// (no runtime dependency, so draconic.scene stays runtime-free).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.scene:manager;

import draconic.foundation;
import :scene;
import :aware;

using namespace draconic::foundation;

export namespace draconic::scene
{

    /// The app-wide list of scene-aware subsystems (physics/audio/render/script). Owned once by
    /// SceneSubsystem; every SceneManager (default + per-instance) fans lifecycle out through the SAME
    /// registry, so a subsystem registered once is injected into every scene regardless of which group
    /// owns it. (Extracted from the old SceneSubsystem broker; behaviour identical.)
    class SceneAwareRegistry
    {
    public:
        void Register(ISceneAware* aware)
        {
            if (aware == nullptr)
            {
                return;
            }
            for (ISceneAware* a : m_aware)
            {
                if (a == aware)
                {
                    return;
                }
            }
            m_aware.PushBack(aware);
        }
        void Unregister(ISceneAware* aware)
        {
            for (usize i = 0; i < m_aware.Size(); ++i)
            {
                if (m_aware[i] == aware)
                {
                    m_aware.RemoveAt(i);
                    return;
                }
            }
        }
        void NotifyCreated(Scene& scene)
        {
            for (ISceneAware* a : m_aware)
            {
                a->OnSceneCreated(scene);
            } // pass 1: inject systems
            for (ISceneAware* a : m_aware)
            {
                a->OnSceneReady(scene);
            } // pass 2: cross-subsystem safe
        }
        void NotifyDestroyed(Scene& scene)
        {
            for (ISceneAware* a : m_aware)
            {
                a->OnSceneDestroyed(scene);
            }
        }

    private:
        Array<ISceneAware*> m_aware;
    };

    class SceneManager
    {
    public:
        explicit SceneManager(SceneAwareRegistry* registry = nullptr) noexcept
            : m_registry(registry)
        {
        }
        void SetAwareRegistry(SceneAwareRegistry* registry) noexcept { m_registry = registry; }

        /// The GROUP time scale - the `instance` term of dt = host x context x GROUP x scene (§5). Default
        /// 1.0, so the editor / default manager reproduces the previous two-level behaviour exactly.
        void SetTimeScale(f32 scale) noexcept { m_timeScale = scale; }
        [[nodiscard]] f32 TimeScale() const noexcept { return m_timeScale; }

        /// The current scene of the group (the multi-scene model, §4.4) - what a game's script time-pairs
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
            UniquePtr<Scene> owned = MakeUnique<Scene>(DefaultAllocator(), name);
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
            // Aware state (component managers etc.) is set up regardless of active, so LoadScene can
            // populate an inactive scene before it is activated.
            if (m_registry != nullptr)
            {
                m_registry->NotifyCreated(*scene);
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
        void BeginFrame(f32 rawHostDt, f32 contextScale, f32 contextStep)
        {
            for (Scene* s : m_active)
            {
                if (contextStep > 0.0f && s->FixedTimeStep() != contextStep)
                {
                    s->SetFixedTiming(contextStep, 4);
                }
                (void)s->AdvanceTime(rawHostDt * contextScale * m_timeScale * s->TimeScale());
            }
        }
        // Variable lane. `contextScaledDt` is already context-scaled by the caller; applies group x scene.
        void Update(f32 contextScaledDt)
        {
            m_updating = true;
            for (Scene* s : m_active)
            {
                s->Update(contextScaledDt * m_timeScale * s->TimeScale());
            }
            m_updating = false;
            ProcessPendingRemoves();
        }

        // Destroy every scene in the group (notifying) - the group teardown.
        void Clear()
        {
            for (usize i = m_scenes.Size(); i-- > 0;)
            {
                if (m_registry != nullptr)
                {
                    m_registry->NotifyDestroyed(*m_scenes[i]);
                }
            }
            m_active.Clear();
            m_pendingRemove.Clear();
            m_current = nullptr;
            m_scenes.Clear(); // UniquePtr frees each Scene
        }

    private:
        void DestroyImmediate(Scene* scene)
        {
            if (m_registry != nullptr)
            {
                m_registry->NotifyDestroyed(*scene);
            }
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

        SceneAwareRegistry* m_registry = nullptr; // shared, app-wide (not owned)
        f32 m_timeScale = 1.0f;                   // the group / instance term
        Scene* m_current = nullptr;               // the group's current scene
        Array<UniquePtr<Scene>> m_scenes;         // ownership
        Array<Scene*> m_active;                   // active (non-owning)
        Array<Scene*> m_pendingRemove;
        bool m_updating = false;
    };

} // namespace draconic::scene
