# AnimatedCrowd - the running dev harness: a windowed DefaultApplication with a scene we
# grow the engine in. Needs the desktop shell + Vulkan GPU factory (to present)
# plus the default-app + scene libraries.

add_executable(AnimatedCrowd main.cpp)

target_link_libraries(AnimatedCrowd PRIVATE
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
    Draconic::ModelImporter
    Draconic::Model
    Draconic::Content
    Draconic::Resource
    Draconic::VFS
    Draconic::Geometry.Resource
    Draconic::Materials.Resource
    Draconic::Texture.Resource
    Draconic::Animation
    Draconic::Engine.Animation
    Draconic::Profiler
    Draconic::Policy)

# Absolute asset paths (cwd-independent), mirroring the VGSandbox font-path pattern:
# the raw model files (read by the loaders) + the cooked-resource output database.
target_compile_definitions(AnimatedCrowd PRIVATE
    DRACONIC_SANDBOX_MODEL_DIR="${CMAKE_SOURCE_DIR}/Data/Assets/models"
    DRACONIC_SANDBOX_OUTPUT_DIR="${CMAKE_SOURCE_DIR}/Data/Output/AnimatedCrowd")   # per-sample cooked-output DB (isolated from other samples)

draconic_copy_sdl3_dll(AnimatedCrowd)
