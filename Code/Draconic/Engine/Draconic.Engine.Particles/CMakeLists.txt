# Draconic::Engine.Particles - `draconic.engine.particles`: scene/render integration for the
# particle system (ECS component + manager + dedicated ParticleRenderer + ParticleSubsystem). Links
# render.subsystem (the register-renderer/register-provider seam), scene, and the CPU particle core.
add_library(Draconic.Engine.Particles STATIC)
add_library(Draconic::Engine.Particles ALIAS Draconic.Engine.Particles)

target_link_libraries(Draconic.Engine.Particles
    PUBLIC Draconic::Foundation Draconic::RHI Draconic::Shaders Draconic::Shaders.System
           Draconic::Runtime Draconic::Scene Draconic::Engine.Scene
           Draconic::Render Draconic::Engine.Render Draconic::Particles
           Draconic::Particles.Resource Draconic::Resource Draconic::Content
           Draconic::Texture Draconic::Texture.Resource
           Draconic::Geometry Draconic::Materials Draconic::Script.Facades
           Draconic::Policy Draconic::Profiler)

target_sources(Draconic.Engine.Particles
    PRIVATE
        ParticleSubsystemImpl.cpp   # DRACONIC_REFLECT_VALUE body (see ParticleComponents.cppm)
    PUBLIC
        FILE_SET particles_subsystem_modules TYPE CXX_MODULES FILES
            ParticleRenderData.cppm
            ParticleRenderer.cppm
            ParticleComponents.cppm
            ParticleSubsystem.cppm
            ParticlesSubsystemModule.cppm)
