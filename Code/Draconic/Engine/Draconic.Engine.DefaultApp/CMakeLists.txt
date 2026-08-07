# Draconic::Engine.DefaultApp - `draconic.engine.defaultapp` (DefaultApplication: the
# opinionated IApplication that registers the standard engine subsystems). Kept in
# its own library so the base client (Draconic::Runtime.Client) never pulls in the
# engine subsystem libraries - only apps that opt into the defaults link this.
# As engine subsystems land, add them as link dependencies HERE.

add_library(Draconic.Engine.DefaultApp STATIC)
add_library(Draconic::Engine.DefaultApp ALIAS Draconic.Engine.DefaultApp)

target_link_libraries(Draconic.Engine.DefaultApp
    PUBLIC Draconic::Foundation Draconic::RHI Draconic::Runtime.Client Draconic::Engine.GameInstance Draconic::Shell Draconic::Graphics
           Draconic::Scene Draconic::Engine.Scene Draconic::Engine.Render Draconic::Engine.Animation
           Draconic::Engine.Particles Draconic::Physics Draconic::Engine.Physics
           Draconic::Audio Draconic::Audio.Resource Draconic::Engine.Audio
           Draconic::Input Draconic::Engine.Input Draconic::Script
           Draconic::Script.Resource Draconic::Engine.Script Draconic::Engine.Integration
           Draconic::Resource Draconic::Content Draconic::Scene.Resource
           Draconic::Geometry.Resource Draconic::Materials.Resource Draconic::Animation.Resource
           Draconic::Particles.Resource Draconic::Input.Resource Draconic::Physics.Resource
           Draconic::Texture.Resource Draconic::Image.Resource Draconic::Model.Resource Draconic::UI.Resource Draconic::Engine.UI
           Draconic::Net Draconic::Net.Manager Draconic::Engine.Net
           Draconic::Profiler Draconic::Policy)

# Script VM backends - Wren (primary) and AngelScript (second). Each is independently toggleable via
# its option; a run resolves the backend by the game script's LANGUAGE, so with both ON a project can
# ship either. Both build on every platform now (AngelScript climbs to web via as_config.h's
# AS_MAX_PORTABILITY). Linked by forward-referenced name (resolves at generate time), so ordering vs
# the Script subdir - which configures AFTER Runtime on the web branch - does not matter. Turn one OFF
# to drop that backend from the player/app entirely; keep at least one for scripted games.
option(DRACONIC_ENABLE_WREN "Build the Wren script backend into DefaultApplication" ON)
option(DRACONIC_ENABLE_ANGELSCRIPT "Build the AngelScript script backend into DefaultApplication" ON)
if(DRACONIC_ENABLE_WREN)
    target_link_libraries(Draconic.Engine.DefaultApp PUBLIC Draconic::Script.Wren)
    target_compile_definitions(Draconic.Engine.DefaultApp PUBLIC DRACONIC_HAS_WREN)
endif()
if(DRACONIC_ENABLE_ANGELSCRIPT)
    target_link_libraries(Draconic.Engine.DefaultApp PUBLIC Draconic::Script.AngelScript)
    target_compile_definitions(Draconic.Engine.DefaultApp PUBLIC DRACONIC_HAS_ANGELSCRIPT)
endif()

target_sources(Draconic.Engine.DefaultApp
    PRIVATE
        DefaultApplicationImpl.cpp   # out-of-line defs (see DefaultApplication.cppm)
    PUBLIC
        FILE_SET runtime_defaultapp_modules TYPE CXX_MODULES FILES
            DefaultApplication.cppm)
