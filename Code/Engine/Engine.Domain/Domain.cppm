// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Domain - the `engine.domain` module.
//
// One declaration per domain, every root a facet of it. A DomainModule is what an engine domain
// (render, physics, input, ...) contributes: its scene facet (the manager installer and the
// reflection registrar a SceneModule carries), its script facade registrar, and the resource
// modules it brings, each optional. An EngineComposition is built from a list of them and answers
// for a facet: the scene composition, the resource types, the factories, the facades. Nobody
// collects anything; a consumer asks the composition for the facet it needs. Engine.Composition
// holds the one list; this library holds the shapes so every domain library can declare against
// them without seeing the list.

module;
#include "Core/Prelude.h"

export module engine.domain;

import foundation.core;
export import foundation.scene;     // the record names SceneModule; a declarer sees it through this module
export import foundation.resource;  // and ResourceModule / ResourceFactorySet

using namespace foundation::core;
namespace scene = foundation::scene;
namespace resource = foundation::resource;

export namespace engine
{
    /// What one engine domain declares. Declared once, in the domain's own library, in an
    /// implementation unit (one instance per process). Every facet is optional: a domain with no
    /// scene content (input) has no installer; a library that is only a facade (`run`) has only
    /// that.
    struct DomainModule
    {
        StringView id = {};
        /// Construction order, as data: the scene composition installs a dependency's systems
        /// first. Facets other than the scene are order-free (registration is idempotent, factories
        /// are keyed by product), so the composition keeps them in declaration order.
        Span<const DomainModule* const> dependsOn = {};
        scene::SceneModule::InstallFn installScene = nullptr;
        scene::SceneModule::RegisterReflectionFn registerReflection = nullptr;
        void (*registerScriptFacade)() = nullptr;
        Span<const resource::ResourceModule* const> resources = {};

        [[nodiscard]] bool HasScene() const noexcept
        {
            return installScene != nullptr || registerReflection != nullptr;
        }
    };

    /// The composition built from a list of domain modules: facets on demand.
    class EngineComposition
    {
    public:
        EngineComposition() = default;

        /// Builds from `modules` in declaration order. The scene facet is a SceneComposition built
        /// from every module with scene content, honouring `dependsOn`; the resource modules are
        /// gathered in module order, each id once.
        static EngineComposition Build(Span<const DomainModule* const> modules)
        {
            EngineComposition composition;
            Array<scene::SceneModule> sceneModules;
            Array<Array<const scene::SceneModule*>> sceneDeps;
            sceneModules.Reserve(modules.Size()); // pointer-stable for the dependency spans below
            sceneDeps.Reserve(modules.Size());
            Array<const DomainModule*> sceneOwners; // parallel to sceneModules
            for (const DomainModule* module : modules)
            {
                composition.m_modules.PushBack(module);
                for (const resource::ResourceModule* resourceModule : module->resources)
                {
                    if (!composition.HasResourceModule(resourceModule->id))
                    {
                        composition.m_resources.PushBack(resourceModule);
                    }
                }
                if (module->HasScene())
                {
                    sceneOwners.PushBack(module);
                }
            }
            // Scene modules first, so a dependency's SceneModule has an address to point at.
            for (const DomainModule* owner : sceneOwners)
            {
                sceneModules.PushBack(
                    scene::SceneModule{owner->id, owner->installScene, owner->registerReflection});
            }
            for (usize i = 0; i < sceneOwners.Size(); ++i)
            {
                Array<const scene::SceneModule*> deps;
                for (const DomainModule* dependency : sceneOwners[i]->dependsOn)
                {
                    for (usize j = 0; j < sceneOwners.Size(); ++j)
                    {
                        if (sceneOwners[j] == dependency)
                        {
                            deps.PushBack(&sceneModules[j]);
                        }
                    }
                }
                sceneDeps.PushBack(Move(deps));
                sceneModules[i] = scene::SceneModule{
                    sceneOwners[i]->id, sceneOwners[i]->installScene,
                    sceneOwners[i]->registerReflection,
                    Span<const scene::SceneModule*>{sceneDeps[i].Data(), sceneDeps[i].Size()}};
            }
            Array<const scene::SceneModule*> ordered;
            for (const scene::SceneModule& module : sceneModules)
            {
                ordered.PushBack(&module);
            }
            composition.m_scene = scene::SceneComposition::Build(
                Span<const scene::SceneModule*>{ordered.Data(), ordered.Size()});
            return composition;
        }

        // ---- facets ----

        /// Every scene manager and settings system, as a SceneComposition (Instantiate a scene,
        /// RegisterReflection); the runtime's SceneSubsystem takes it as is.
        [[nodiscard]] const scene::SceneComposition& Scene() const noexcept { return m_scene; }
        [[nodiscard]] Span<const DomainModule* const> Modules() const noexcept
        {
            return Span<const DomainModule* const>{m_modules.Data(), m_modules.Size()};
        }
        /// The resource modules every domain brings, each id once, in domain order.
        [[nodiscard]] Span<const resource::ResourceModule* const> ResourceModules() const noexcept
        {
            return Span<const resource::ResourceModule* const>{m_resources.Data(), m_resources.Size()};
        }
        /// Every factory description of every resource module: what the composition can create,
        /// readable without creating anything (the scene format reference joins on it).
        template <typename Fn>
        void ForEachFactoryDescription(Fn&& fn) const
        {
            for (const resource::ResourceModule* module : m_resources)
            {
                for (const resource::ResourceFactoryDesc& desc : module->Factories())
                {
                    fn(*module, desc);
                }
            }
        }
        [[nodiscard]] usize FactoryDescriptionCount() const noexcept
        {
            usize count = 0;
            for (const resource::ResourceModule* module : m_resources)
            {
                count += module->factoryCount;
            }
            return count;
        }
        /// Registers every resource module's types (cooked records and products, by type name).
        void RegisterResourceTypes() const
        {
            for (const resource::ResourceModule* module : m_resources)
            {
                module->RegisterTypes();
            }
        }
        /// Registers every domain's component reflection (the scene facet's registrars, plus the
        /// runtime contributions the scene composition knows about).
        void RegisterReflection() const { m_scene.RegisterReflection(); }
        /// Registers every domain's script facade (metadata only; idempotent).
        void RegisterScriptFacades() const
        {
            for (const DomainModule* module : m_modules)
            {
                if (module->registerScriptFacade != nullptr)
                {
                    module->registerScriptFacade();
                }
            }
        }
        /// Creates every factory the composition describes and `services` allows, into `set`.
        void CreateFactories(resource::ResourceFactorySet& set, IAllocator& allocator,
                             const resource::IResourceServices& services) const
        {
            set.Create(ResourceModules(), allocator, services);
        }

    private:
        [[nodiscard]] bool HasResourceModule(StringView id) const noexcept
        {
            for (const resource::ResourceModule* module : m_resources)
            {
                if (module->id == id)
                {
                    return true;
                }
            }
            return false;
        }

        Array<const DomainModule*> m_modules;
        Array<const resource::ResourceModule*> m_resources;
        scene::SceneComposition m_scene;
    };
}
