# ParticleFX - the particle-system showcase (CPU-sim fountain drawn by the dedicated ParticleRenderer).
# Same host set as the other samples + the particle core and its scene/render integration.

add_executable(ParticleFX main.cpp)

target_link_libraries(ParticleFX PRIVATE
    Draconic::Foundation
    Draconic::Runtime
    Draconic::Runtime.Client
    Draconic::Shell.Desktop Draconic::Runtime.Desktop
    Draconic::Graphics
    Draconic::Graphics.Gpu
    Draconic::Engine.DefaultApp
    Draconic::Scene
    Draconic::Engine.Scene
    Draconic::Engine.Render
    Draconic::Imgui
    Draconic::Geometry
    Draconic::Materials
    Draconic::Particles
    Draconic::Engine.Particles
    Draconic::Particles.Resource       # cooked ParticleEffectResource + factory
    Draconic::Particles.Editor         # ParticleEffectAsset + bake (sample references editor for the demo)
    Draconic::Editor Draconic::Content Draconic::Resource Draconic::VFS
    Draconic::Policy)

# Per-sample cooked-output DB (isolated), matching the other samples' pattern.
target_compile_definitions(ParticleFX PRIVATE
    DRACONIC_PARTICLEFX_OUTPUT_DIR="${CMAKE_SOURCE_DIR}/Data/Output/ParticleFX")

draconic_copy_sdl3_dll(ParticleFX)
