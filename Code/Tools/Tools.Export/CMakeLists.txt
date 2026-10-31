# Tools.Export - packages a project into a shippable dist: cook, stage the scenes, and write
# Content.pak and player.xml beside Engine.Player.
add_executable(Tools.Export Main.cpp)
add_dependencies(Tools.Export Engine.Player)   # stages the player exe when present
target_link_libraries(Tools.Export PRIVATE
    Foundation::Core Foundation::Scene.Resource
    Pipeline::Registration # the composition root: every builder/type registration
    Pipeline::Core Pipeline::Cook Editor::Project
    Pipeline::Texture Pipeline::Fonts Pipeline::Image Foundation::Image.Resource Pipeline::Geometry
    Pipeline::Animation Pipeline::Materials Pipeline::Shaders
    Pipeline::Particles Foundation::Input Foundation::Input.Resource Pipeline::Input Pipeline::ModelImporter
    Engine::Composition # the full manager set and component reflection for the transcode scratch
    Engine::Composition # RegisterAllScriptFacades: the complete engine facade surface the
                          # script cooks compile against (run/ui/physics/audio/... facades)
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
