# Draconic::Engine.Physics - `draconic.engine.physics`: scene integration
# (components + per-scene world + sync + interpolation + debug draw).
add_library(Draconic.Engine.Physics STATIC)
add_library(Draconic::Engine.Physics ALIAS Draconic.Engine.Physics)

target_link_libraries(Draconic.Engine.Physics
    PUBLIC Draconic::Foundation Draconic::Runtime Draconic::Scene Draconic::Engine.Scene
           Draconic::Physics Draconic::Physics.Resource Draconic::Resource Draconic::Script
           Draconic::Script.Facades Draconic::Engine.Render Draconic::Policy)

target_sources(Draconic.Engine.Physics
    PRIVATE
        PhysicsSubsystemImpl.cpp   # implementation unit: render-frame drive (render dep stays out of the interface)
    PUBLIC
        FILE_SET physics_subsystem_modules TYPE CXX_MODULES FILES
            PhysicsSubsystem.cppm
            PhysicsComponents.cppm)

