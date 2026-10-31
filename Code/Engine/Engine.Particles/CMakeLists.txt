# Engine::Particles - `engine.particles`: scene/render integration for the
# particle system (ECS component, manager, a dedicated ParticleRenderer, ParticleSubsystem). Links
# render.subsystem (to register the renderer and provider), scene, and the CPU particle core.
util_add_engine_library(Engine.Particles ALIAS Engine::Particles)
target_link_libraries(Engine.Particles
    PUBLIC Foundation::Core Foundation::RHI Foundation::Shaders Foundation::Shaders.System
           Foundation::Runtime Foundation::Scene Engine::Scene
           Foundation::Render Engine::Render Foundation::Particles
           Foundation::Particles.Resource Foundation::Resource Foundation::Content
           Foundation::Texture Foundation::Texture.Resource
           Foundation::Geometry Foundation::Materials Foundation::Script.Facades
           Engine::Domain Foundation::Policy Foundation::Profiler)

target_sources(Engine.Particles
    PRIVATE
        ParticleSubsystemImpl.cpp   # REFLECT_VALUE body (see ParticleComponents.cppm)
    PUBLIC
        FILE_SET particles_subsystem_modules TYPE CXX_MODULES FILES
            ParticleRenderData.cppm
            ParticleRenderer.cppm
            ParticleComponents.cppm
            ParticleSubsystem.cppm
            ParticlesSubsystemModule.cppm)
