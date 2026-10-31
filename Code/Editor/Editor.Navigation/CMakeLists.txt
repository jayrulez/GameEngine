# Editor::Navigation - `editor.navigation`: the navmesh authoring tools (tooling).
# The "Bake Navigation" action: collect the scene's static geometry touching a zone's box (in
# zone-local space), bake it with Recast, and write the result into the zone's NavigationZoneAsset
# sidecar. Editor-only; never linked by the runtime.
util_add_engine_library(Editor.Navigation ALIAS Editor::Navigation)
target_link_libraries(Editor.Navigation PUBLIC
    Foundation::Core Foundation::Scene Foundation::Content
    Foundation::Navigation Foundation::Navigation.Resource Editor::Core Foundation::Settings
    Engine::Navigation Pipeline::Navigation Foundation::Policy)
target_sources(Editor.Navigation
    PRIVATE
        NavigationBakeImpl.cpp   # bake logic and heavy imports (kept out of the interface unit)
    PUBLIC
        FILE_SET editor_navigation_modules TYPE CXX_MODULES FILES NavigationBake.cppm)
