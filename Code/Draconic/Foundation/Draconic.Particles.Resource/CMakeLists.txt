# Draconic::Particles.Resource - `draconic.particles.resource`: the cooked ParticleEffectResource
# (runtime input), its bidirectional serializer, and its resource factory. Layers on the runtime
# particle types + the content DB + resource manager. See docs/design/particles-authoring.md.
add_library(Draconic.Particles.Resource STATIC)
add_library(Draconic::Particles.Resource ALIAS Draconic.Particles.Resource)

target_link_libraries(Draconic.Particles.Resource
    PUBLIC Draconic::Foundation Draconic::Particles Draconic::Content Draconic::Resource
           Draconic::Texture Draconic::Texture.Resource Draconic::Policy)

target_sources(Draconic.Particles.Resource
    PUBLIC
        FILE_SET particles_resource_modules TYPE CXX_MODULES FILES
            ParticleEffectResource.cppm)

