# ParticleFX - the particle-system showcase (CPU-sim fountain drawn by the dedicated ParticleRenderer).
# Same host set as the other samples, plus the particle core and its scene/render integration.

add_executable(ParticleFX main.cpp)

target_link_libraries(ParticleFX PRIVATE
    Foundation::Core
    Foundation::Runtime
    Foundation::Runtime.Client
    Foundation::Shell.Desktop Foundation::Runtime.Desktop
    Foundation::Graphics
    Foundation::Graphics.Gpu
    Engine::DefaultApp
    Engine::Composition
    Foundation::Scene
    Engine::Scene
    Engine::Render
    Foundation::Geometry
    Foundation::Materials
    Foundation::Particles
    Engine::Particles
    Foundation::Particles.Resource       # cooked ParticleEffectResource and factory
    Pipeline::Particles         # ParticleEffectAsset and its bake (the sample cooks its demo effect)
    Pipeline::Core Foundation::Content Foundation::Resource Foundation::VFS
    Foundation::Policy)

util_copy_sdl3_dll(ParticleFX)

# Dear ImGui, which the app loads as a plugin (its part linked, and the registry listing it).
util_link_plugins(ParticleFX ROLE player PLUGINS imgui)
