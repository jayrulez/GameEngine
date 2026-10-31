# Engine::DefaultApp - `engine.defaultapp` (DefaultApplication: the
# opinionated IApplication that registers the standard engine subsystems). Kept in
# its own library so the base client (Foundation::Runtime.Client) never pulls in the
# engine subsystem libraries; only apps that opt into the defaults link this.
# A new engine subsystem is added as a link dependency here.

util_add_engine_library(Engine.DefaultApp ALIAS Engine::DefaultApp)
target_link_libraries(Engine.DefaultApp
    PUBLIC Foundation::Core Foundation::RHI Foundation::Runtime.Client Engine::GameInstance Foundation::Shell Foundation::Graphics
           Foundation::VFS
           Foundation::Scene Engine::Scene Engine::Render Engine::Animation
           Engine::Particles Engine::Terrain Engine::Vegetation Foundation::Physics Engine::Physics Engine::Navigation Foundation::Navigation.Resource
           Foundation::Audio Foundation::Audio.Resource Engine::Audio
           Foundation::Fonts.Resource
           Foundation::Input Engine::Input Foundation::Script
           Foundation::Script.Resource Engine::Script Engine::Integration
           Foundation::Resource Foundation::Content Foundation::Scene.Resource
           Foundation::Geometry.Resource Foundation::Materials.Resource Foundation::Animation.Resource
           Foundation::PropertyAnimation.Resource
           Foundation::Particles.Resource Foundation::Input.Resource Foundation::Physics.Resource
           Foundation::Texture.Resource Foundation::Image.Resource Foundation::Model.Resource
           Foundation::Heightfield.Resource Foundation::Terrain.Resource Foundation::Vegetation.Resource
            Foundation::Net Foundation::Net.Manager Engine::Net
            Foundation::Profiler Foundation::Profiler.GUI Foundation::Policy Engine::Composition 
            Foundation::Image Foundation::Image.IO Foundation::RHI.Capture)

# Script VM backends built into the default application: AngelScript and Luau. Each is switched by
# its option; a run resolves the backend by the game script's language, so with both on a project
# can ship either. Both build on every platform (AngelScript reaches the web through as_config.h's
# AS_MAX_PORTABILITY). Turn one off to drop that backend from the player and app entirely. The
# OPTION_ENABLE_* flags and OPTION_HAS_<backend> macros are global (root CMakeLists), and the
# target-or-empty ${SCRIPTLIB_*} aliases link whichever are enabled; the root guarantees at least one.
target_link_libraries(Engine.DefaultApp PUBLIC
    ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU})

target_sources(Engine.DefaultApp
    PRIVATE
        DefaultApplicationImpl.cpp   # out-of-line defs (see DefaultApplication.cppm)
    PUBLIC
        FILE_SET runtime_defaultapp_modules TYPE CXX_MODULES FILES
            DefaultApplication.cppm
            ProfilerOverlayHost.cppm)

target_link_libraries(Engine.DefaultApp PUBLIC Engine::GUI) # the engine GUI
