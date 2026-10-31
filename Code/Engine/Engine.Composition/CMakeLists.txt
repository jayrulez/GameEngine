# Engine::Composition - `engine.composition`: the composition root. The one list of engine
# domains (each declared once in its own library as a DomainModule) and the facets a consumer
# asks it for: the scene composition, the component reflection, the resource types, the
# factories, the script facades. Every host composes from here. Headless-safe: managers are value pools, settings systems construct
# inert, facade registration is metadata only.

util_add_engine_library(Engine.Composition ALIAS Engine::Composition)
target_link_libraries(Engine.Composition PUBLIC
    Foundation::Core Foundation::Content Foundation::Scene Foundation::Scene.Resource Foundation::Resource
    Foundation::Script.Facades
    Engine::Domain
    Engine::Render Engine::Animation Engine::Particles Engine::Physics Engine::Terrain Engine::Vegetation Engine::Spline Engine::Navigation
    Engine::Audio Engine::Script Engine::Input Engine::GameInstance
    Foundation::Net.Replication Foundation::Net.Manager Engine::Net
    Foundation::Policy)
target_link_libraries(Engine.Composition PUBLIC Engine::GUI) # the GUI domain

target_sources(Engine.Composition
    PRIVATE
        CompositionImpl.cpp
        SceneExportSupport.cpp
    PUBLIC
        FILE_SET engine_composition_modules TYPE CXX_MODULES FILES
            Composition.cppm)
