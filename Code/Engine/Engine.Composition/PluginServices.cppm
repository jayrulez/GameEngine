// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Composition - :plugin_services partition.
//
// What an Engine part of a plugin adds to the engine, through the service its host provides
// (PluginContext::Find<EnginePluginServices>): a domain (its resource modules, reflection and
// facade join the full composition) and a scene manager (it reaches live scenes and new ones).
// The service records what each plugin added, so the plugin host's unload takes it all out again,
// in reverse. A plugin's per-run state belongs to the runs themselves (foundation.run: a listener
// on the run manager adds it to each run as the run is made).

module;
#include "Core/Prelude.h"

export module engine.composition:plugin_services;

import foundation.core;
import foundation.runtime;
import foundation.scene;
import engine.domain;

using namespace foundation::core;

export namespace engine
{
    class EnginePluginServices final : public foundation::runtime::IRegistrationRecorder
    {
    public:
        explicit EnginePluginServices(IAllocator& allocator) : m_entries(allocator) {}
        ~EnginePluginServices() override = default;

        EnginePluginServices(const EnginePluginServices&) = delete;
        EnginePluginServices& operator=(const EnginePluginServices&) = delete;

        /// Hands this service to `host`'s plugins and records what they add through it, and the
        /// scene managers they contribute. Before the host loads; this outlives the host.
        [[nodiscard]] bool Attach(foundation::runtime::PluginHost& host);

        /// Adds a domain to the full composition (AddPluginDomain). False, with nothing added,
        /// for a domain with a scene installer: scene managers come through AddSceneManager.
        [[nodiscard]] bool AddDomain(const DomainModule& module);
        /// Adds a scene manager to every live scene and every scene created after.
        void AddSceneManager(const foundation::scene::SceneModuleContributions::Contribution& contribution);

        // IRegistrationRecorder: armed around each part's load, so what it added is recorded
        // against it, and reversed when it unloads.
        void Arm(Array<TypeId>& sink) override { m_sink = &sink; }
        void Disarm() override { m_sink = nullptr; }
        void Reverse(TypeId id) override;

    private:
        struct Entry
        {
            u64 token;  // this service's, what the recording keeps
            u64 domain; // the composition's token
        };

        Array<Entry> m_entries;
        Array<TypeId>* m_sink = nullptr;
        u64 m_nextToken = 1;
    };
}
