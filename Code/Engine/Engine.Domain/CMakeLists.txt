# Engine::Domain - `engine.domain`: what one engine domain declares (DomainModule: its scene
# facet, its reflection, its script facade, the resource modules it brings) and the composition
# built from a list of them (EngineComposition, which builds facets on demand). Every
# Engine.<Domain> library declares its module against this; Engine.Composition lists them once.
# Depends only on Foundation::Scene (SceneModule/SceneComposition) and Foundation::Resource
# (ResourceModule, ResourceFactorySet).

util_add_engine_library(Engine.Domain ALIAS Engine::Domain)
target_link_libraries(Engine.Domain PUBLIC
    Foundation::Core Foundation::Scene Foundation::Resource Foundation::Policy)

target_sources(Engine.Domain
    PUBLIC
        FILE_SET engine_domain_modules TYPE CXX_MODULES FILES
            Domain.cppm)
