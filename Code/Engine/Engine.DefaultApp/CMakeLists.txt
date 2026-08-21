# Engine::DefaultApp - `engine.defaultapp` (DefaultApplication: the
# opinionated IApplication that registers the standard engine subsystems). Kept in
# its own library so the base client (Foundation::Runtime.Client) never pulls in the
# engine subsystem libraries - only apps that opt into the defaults link this.
# As engine subsystems land, add them as link dependencies HERE.

add_library(Engine.DefaultApp STATIC)
add_library(Engine::DefaultApp ALIAS Engine.DefaultApp)

target_link_libraries(Engine.DefaultApp
    PUBLIC Foundation::Core Foundation::RHI Foundation::Runtime.Client Engine::GameInstance Foundation::Shell Foundation::Graphics
           Foundation::Scene Engine::Scene Engine::Render Engine::Animation
           Engine::Particles Foundation::Physics Engine::Physics Engine::Navigation Foundation::Navigation.Resource
           Foundation::Audio Foundation::Audio.Resource Engine::Audio
           Foundation::Fonts.Resource
           Foundation::Input Engine::Input Foundation::Script
           Foundation::Script.Resource Engine::Script Engine::Integration
           Foundation::Resource Foundation::Content Foundation::Scene.Resource
           Foundation::Geometry.Resource Foundation::Materials.Resource Foundation::Animation.Resource
           Foundation::PropertyAnimation.Resource
           Foundation::Particles.Resource Foundation::Input.Resource Foundation::Physics.Resource
           Foundation::Texture.Resource Foundation::Image.Resource Foundation::Model.Resource Foundation::UI.Resource Engine::UI Engine::UI.Script Foundation::UI.Gamekit
            Foundation::Net Foundation::Net.Manager Engine::Net
            Foundation::Profiler Foundation::Policy Engine::SceneSurface)

# Script VM backends - AngelScript and Luau. Each is independently toggleable via
# its option; a run resolves the backend by the game script's LANGUAGE, so with both ON a project can
# ship either. Both build on every platform now (AngelScript climbs to web via as_config.h's
# AS_MAX_PORTABILITY). Linked by forward-referenced name (resolves at generate time), so ordering vs
# the Script subdir - which configures AFTER Runtime on the web branch - does not matter. Turn one OFF
# to drop that backend from the player/app entirely; keep at least one for scripted games.
# Script backends built into the default application. The OPTION_ENABLE_* flags + OPTION_HAS_<backend>
# presence macros are now GLOBAL (root CMakeLists); link whichever are enabled via the target-or-empty
# ${SCRIPTLIB_*} aliases. At least one is guaranteed on (the root enforces it).
target_link_libraries(Engine.DefaultApp PUBLIC
    ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU})

target_sources(Engine.DefaultApp
    PRIVATE
        DefaultApplicationImpl.cpp   # out-of-line defs (see DefaultApplication.cppm)
    PUBLIC
        FILE_SET runtime_defaultapp_modules TYPE CXX_MODULES FILES
            DefaultApplication.cppm)
