# Draconic::Particles.Editor - `draconic.particles.editor`: the edit-time ParticleEffectAsset + its
# builder (the bake). Tooling only - the runtime/app never links this. See
# docs/design/particles-authoring.md.
add_library(Draconic.Particles.Editor STATIC)
add_library(Draconic::Particles.Editor ALIAS Draconic.Particles.Editor)

target_link_libraries(Draconic.Particles.Editor
    PUBLIC Draconic::Foundation Draconic::Editor Draconic::Particles Draconic::Particles.Resource
           Draconic::Content Draconic::Policy)

target_sources(Draconic.Particles.Editor
    PUBLIC
        FILE_SET particles_editor_modules TYPE CXX_MODULES FILES
            ParticleEffectAsset.cppm)

