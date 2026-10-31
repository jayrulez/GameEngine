# Engine::Physics - `engine.physics`: scene integration
# (components, a per-scene world, sync, interpolation, debug draw).
util_add_engine_library(Engine.Physics ALIAS Engine::Physics)
target_link_libraries(Engine.Physics
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Scene Engine::Scene
           Foundation::Physics Foundation::Physics.Resource Foundation::Resource Foundation::Script
           Foundation::Script.Facades Engine::Render Foundation::Profiler Engine::Domain Foundation::Policy
           Foundation::Heightfield)

target_sources(Engine.Physics
    PRIVATE
        PhysicsSubsystemImpl.cpp   # implementation unit: the render-frame drive (the render dependency stays out of the interface)
    PUBLIC
        FILE_SET physics_subsystem_modules TYPE CXX_MODULES FILES
            PhysicsSubsystem.cppm
            PhysicsComponents.cppm)

