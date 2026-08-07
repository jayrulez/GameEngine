# Sandbox - the running dev harness: a windowed DefaultApplication with a scene we
# grow the engine in. Needs the desktop shell + Vulkan GPU factory (to present)
# plus the default-app + scene libraries.

add_executable(Sandbox main.cpp)

target_link_libraries(Sandbox PRIVATE
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
    Draconic::Engine.Animation
    Draconic::Particles
    Draconic::Engine.Particles
    Draconic::Imgui
    Draconic::Image
    Draconic::Image.IO
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
    Draconic::Texture.Editor
    Draconic::Animation
    Draconic::Policy)

# Absolute asset paths (cwd-independent), mirroring the VGSandbox font-path pattern:
# the raw model files (read by the loaders) + the cooked-resource output database.
target_compile_definitions(Sandbox PRIVATE
    DRACONIC_SANDBOX_MODEL_DIR="${CMAKE_SOURCE_DIR}/Data/Assets/models"
    DRACONIC_SANDBOX_ENV_DIR="${CMAKE_SOURCE_DIR}/Data/Assets/environment"
    DRACONIC_SANDBOX_IMAGE_DIR="${CMAKE_SOURCE_DIR}/Data/Assets/images"
    DRACONIC_SANDBOX_OUTPUT_DIR="${CMAKE_SOURCE_DIR}/Data/Output/Sandbox")   # per-sample cooked-output DB (isolated from other samples)

draconic_copy_sdl3_dll(Sandbox)
