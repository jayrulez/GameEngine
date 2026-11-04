# Tools.Export - packages a project into a shippable dist: cook, stage the scenes, and write
# Content.pak and player.xml beside Engine.Player.
add_executable(Tools.Export Main.cpp)
util_link_plugins(Tools.Export ROLE export PLUGINS ${UTIL_HOST_PLUGINS}) # what a project may enable
add_dependencies(Tools.Export Engine.Player)   # stages the player exe when present
target_link_libraries(Tools.Export PRIVATE
    Foundation::Core Foundation::Scene.Resource
    Pipeline::Registration # the composition root: every builder/type registration
    Pipeline::Core Pipeline::Cook Editor::Project
    Pipeline::Texture Pipeline::Fonts Pipeline::Image Foundation::Image.Resource Pipeline::Geometry
    Pipeline::Animation Pipeline::Materials Pipeline::Shaders
    Pipeline::Particles Foundation::Input Foundation::Input.Resource Pipeline::Input Pipeline::ModelImporter
    # The full manager set and component reflection for the transcode scratch, and
    # RegisterAllScriptFacades, the engine facade surface the script cooks compile against.
    Engine::Composition
    Foundation::Physics Foundation::Physics.Resource Pipeline::Physics
    Foundation::Audio Foundation::Audio.Resource Pipeline::Audio
    # Both script backends and their cooks come transitively from Pipeline::Registration (PUBLIC),
    # so export cooks scripts of either language (AngelScript, Luau) without naming a backend here.
    Foundation::Script Foundation::Script.Resource Foundation::Script.Facades
    Pipeline::Script
)


# The command line parses: --data-root (named in the usage and the no-data-root error) is accepted,
# so a missing project is the failure reported, not an unknown option.
add_test(NAME Tools.Export.Arguments
         COMMAND Tools.Export ${CMAKE_BINARY_DIR}/no-such-project --data-root ${CMAKE_SOURCE_DIR}/Data)
set_tests_properties(Tools.Export.Arguments PROPERTIES
                     PASS_REGULAR_EXPRESSION "failed to open project"
                     FAIL_REGULAR_EXPRESSION "unknown option")

# Its runtime dependencies beside it, listed in Tools.Export.runtime-libs (the editor distribution ships
# the tool with them): the engine's own libraries in a shared build, and DXC, which its shader cooks
# load at run time.
util_copy_runtime_deps(Tools.Export EXTRA ${BUILDSYSTEM_DXC_LIB} ${BUILDSYSTEM_NOTICES_FILE})
