# Pipeline::Particles - `particles.pipeline`: the edit-time ParticleEffectAsset and its
# builder (the bake). Tooling only; the runtime and app never link this.
util_add_engine_library(Particles.Pipeline ALIAS Pipeline::Particles)
target_link_libraries(Particles.Pipeline
    PUBLIC Foundation::Core Pipeline::Core Foundation::Particles Foundation::Particles.Resource
           Foundation::Content Foundation::Policy)

target_sources(Particles.Pipeline
    PRIVATE
        ParticlesCreatorsImpl.cpp # File > New, asset_create
    PUBLIC
        FILE_SET particles_editor_modules TYPE CXX_MODULES FILES
            ParticleEffectAsset.cppm)

