# Foundation::Particles.Resource - `foundation.particles.resource`: the cooked ParticleEffectResource
# (runtime input), its bidirectional serializer, and its resource factory. Layers on the runtime
# particle types, the content DB and the resource manager.
util_add_engine_library(Particles.Resource ALIAS Foundation::Particles.Resource)
target_link_libraries(Particles.Resource
    PUBLIC Foundation::Core Foundation::Particles Foundation::Content Foundation::Resource
           Foundation::Texture Foundation::Texture.Resource
           Foundation::Geometry Foundation::Geometry.Resource
           Foundation::Materials Foundation::Materials.Resource Foundation::Policy)

target_sources(Particles.Resource
    PUBLIC
        FILE_SET particles_resource_modules TYPE CXX_MODULES FILES
            ParticleEffectResource.cppm)

