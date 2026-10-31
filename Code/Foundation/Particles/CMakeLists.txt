# Foundation::Particles - `foundation.particles`, the CPU particle runtime: the SoA stream
# container, initializer and behavior modules, effect/system/emitter/instance, and the CPU
# simulator. Pure logic over Core; the cooked resource, the scene and render integration and
# the editor's page layer on top.
util_add_engine_library(Particles ALIAS Foundation::Particles)
target_link_libraries(Particles
    PUBLIC Foundation::Core Foundation::Policy)

target_sources(Particles
    PRIVATE
        ParticleModulesImpl.cpp   # module and range-type reflection bodies (kept out of interfaces for GCC)
        ParticleEffectReflectionImpl.cpp # effect/system/emitter reflection bodies (kept out of interfaces for GCC)
    PUBLIC
        FILE_SET particles_modules TYPE CXX_MODULES FILES
            ParticleTypes.cppm
            ParticleStreams.cppm
            ParticleModules.cppm
            ParticleEffect.cppm
            ParticlesModule.cppm)

