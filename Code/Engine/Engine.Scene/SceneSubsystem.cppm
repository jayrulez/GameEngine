// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Scene - `engine.scene`.
///
/// The application-level scene driver: it owns the pure scene registry
/// (foundation.scene:composition's `SceneRegistry`) and drives every registered manager's per-frame
/// update and fixed update (UpdateOrder -500, so scenes tick before rendering reads them). It owns no
/// scenes itself: there is no implicit "default" group. Every owner of scenes (a GameInstance, an
/// editor page) creates its own SceneManager and registers it here. The scene lifecycle and tick logic
/// live in SceneManager; this class only fans the Context time scale and fixed step to the managers
/// (foundation.scene stays runtime-free: the manager takes those as parameters).
///
/// Scene assembly is declarative: each registered manager gets an installer that assembles new scenes
/// from the registry's `SceneComposition` (the single source of truth), then fans the `SystemsReady`
/// observer stage; teardown fans `Destroying`. Reactive wiring (cross-subsystem per-scene state) is
/// delivered through the observer stages; there is no other injection path.
///
/// Scratch and headless consumers that never tick on the Context lane (the editor's export transcode
/// and reachability scan) build a Scene directly through `Engine.Composition::FullSceneComposition()`, not this
/// subsystem.

module;
#include "Core/Prelude.h"

export module engine.scene;

import foundation.core;
import foundation.runtime;
import foundation.scene;
import foundation.scene.resource; // ResolveAllUnresolvedComponents (preserved records)

using namespace foundation::core;
using namespace foundation::scene;

export namespace engine::scene
{

    // The PluginHost recorder for scene-manager contributions: armed
    // around a plugin's OnLoad, it records the managers the plugin contributes and reverses
    // them on unload. One per process (defined in SceneSubsystemImpl.cpp).
    class SceneContributionRecorder final : public foundation::runtime::IRegistrationRecorder
    {
    public:
        void Arm(Array<TypeId>& sink) override;
        void Disarm() override;
        void Reverse(TypeId id) override;
    };
    [[nodiscard]] SceneContributionRecorder& GlobalSceneContributionRecorder() noexcept;

    class SceneSubsystem final : public foundation::runtime::Subsystem
    {
    public:
        [[nodiscard]] i32 UpdateOrder() const noexcept override
        {
            return -500;
        } // scenes tick early

        // While registered, this subsystem is the contribution registry's live-scene sink:
        // a manager a plugin contributes (or withdraws) after scenes exist is applied to every
        // live scene here, and preserved records of its type resolve into it.
        void OnRegister(foundation::runtime::SubsystemManager* manager) override
        {
            Subsystem::OnRegister(manager);
            SceneModuleContributions::Global().SetLiveSceneSink(
                [](void* ctx, void (*fn)(void*, Scene&), void* fnContext)
                {
                    static_cast<SceneSubsystem*>(ctx)->m_scenes.ForEachScene(
                        [&](Scene& scene) { fn(fnContext, scene); });
                },
                this);
            SceneModuleContributions::Global().SetLiveInstallHook(
                [](void*, Scene& scene) { ResolveAllUnresolvedRecords(scene); }, nullptr);
        }
        void OnUnregister() override
        {
            SceneModuleContributions::Global().SetLiveSceneSink(nullptr, nullptr);
            SceneModuleContributions::Global().SetLiveInstallHook(nullptr, nullptr);
            Subsystem::OnUnregister();
        }

        // ---- observer broker (reactive scene-lifecycle stages) ----

        void RegisterObserver(ISceneObserver* observer, SceneLifecycleStage stage)
        {
            m_scenes.AddObserver(observer, stage);
        }
        void UnregisterObserver(ISceneObserver* observer) { m_scenes.RemoveObserver(observer); }

        // ---- composition (the declarative single source of truth) ----

        /// Install the declarative assignment of modules every registered manager's CreateScene assembles
        /// from. A manager registered before SetComposition reads the live composition at create time, so
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
        /// destroy and clear fan Destroying. The subsystem holds a `SceneManager*` (a scene-library type): it
        /// never learns about its owner (a GameInstance or editor page), so the dependency stays down. The owner
        /// unregisters before destroying the manager.
        void RegisterManager(SceneManager* manager)
        {
            if (manager == nullptr)
            {
                return;
            }
            m_scenes.RegisterManager(manager); // dedups; adds to the tick list

            // The installer closes over this subsystem: read the live composition at create time. The
            // subsystem outlives every registered manager (the owner unregisters and clears its scenes before
            // subsystem teardown), so the `this` capture is safe under the same borrowed-pointer discipline
            // the manager list already relies on.
            SceneManager::SceneInstaller installer{
                [this](Scene& scene)
                {
                    // Composing fires before assembly (the stage is a pre-install hook),
                    // SystemsReady after every module installed.
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

        /// The pure scene registry backing this subsystem (manager list, observer list, sweeps and
        /// lane fan-out); exposed so owners and tooling can reach the composition and observer state.
        [[nodiscard]] SceneRegistry& Registry() noexcept { return m_scenes; }

        /// Visit every registered manager (tooling sweeps that must reach all live scenes across owners).
        template <typename Fn>
        void ForEachManager(Fn&& fn)
        {
            m_scenes.ForEachManager(fn);
        }

        /// Visit every live scene across all registered managers: a read-only registry sweep (not scene
        /// ownership) for prefab-instance rebuilds after a template save and export scans. Each owner still owns
        /// its own scenes on its own manager.
        template <typename Fn>
        void ForEachScene(Fn&& fn)
        {
            m_scenes.ForEachScene(fn);
        }

        // ---- subsystem frame phases (drive every registered manager; Context factors applied here) ----

        // Fixed stepping is per scene (each scene owns a FixedStepper): BeginFrame runs it so fixed-rate
        // state (physics poses and alpha) is fresh before any subsystem's Update reads it. BeginFrame gets the
        // raw host dt (context scale applied inside the manager); Update gets context-scaled dt.
        void BeginFrame(f32 deltaTime) override
        {
            // The one legal crossing point between the subsystem manager (plain floats) and
            // scene::FrameTime (by the layering rules runtime never sees FrameTime; this constructs
            // it and hands it down). Captured for Update so both lanes see one consistent chain per
            // frame.
            m_frameTime = FrameTime(deltaTime,
                                    Manager() != nullptr ? Manager()->TimeScale() : 1.0f,
                                    /*group*/ 1.0f, /*scene*/ 1.0f,
                                    Manager() != nullptr ? Manager()->FixedTimeStep() : 0.0f);
            m_scenes.BeginFrame(m_frameTime);
        }
        void Update(f32 /*contextScaledDt: the captured FrameTime replaces it*/) override
        {
            m_scenes.Update(m_frameTime);
        }
        // OnShutdown: nothing to clear; every manager is cleared by its owner.

    private:
        SceneRegistry m_scenes; // pure manager, observer and sweep state (foundation.scene:composition)
        FrameTime m_frameTime; // built in BeginFrame; Update reuses the same chain
    };

} // namespace engine::scene