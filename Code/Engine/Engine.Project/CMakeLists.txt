# Engine::Project - `engine.project`, the runtime-side project definition (manifest and
# layout constants). Editor-free, because shipping binaries link this and never the editor.
util_add_engine_library(Engine.Project ALIAS Engine::Project)
target_link_libraries(Engine.Project
    PUBLIC Foundation::Core Foundation::VFS Foundation::Xml.Serialization Foundation::Policy)

target_sources(Engine.Project
    PRIVATE
        ProjectSettingsImpl.cpp
    PUBLIC
        FILE_SET project_modules TYPE CXX_MODULES FILES
            ProjectModule.cppm)

# Engine version: the single source of truth is project(VERSION) in the root CMakeLists.
# Scoped to this target (not the global policy) so a version bump only rebuilds engine.project and
# its importers, not the whole engine. ProjectModule.cppm bakes these into kEngineVersion*, which
# flows to saved project manifests, export template.xml, and the version report.
target_compile_definitions(Engine.Project PRIVATE
    ENGINE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    ENGINE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    ENGINE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    ENGINE_VERSION_STRING=u8\"${PROJECT_VERSION}\")
