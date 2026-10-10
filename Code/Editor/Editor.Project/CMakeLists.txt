# Editor::Project - `editor.project` (the headless project half of the editor domain: the
# open project, cook service, export, jobs, log buffer, project registry + manager, the root
# allocator seam). What a host without an editor stands on: the MCP stdio server, the cook
# and export tools. Tooling only - the runtime never links this.

util_add_engine_library(Editor.Project ALIAS Editor::Project)
target_link_libraries(Editor.Project
    PUBLIC Pipeline::Importer Foundation::Core Foundation::VFS Foundation::VFS.Pak Foundation::Content Foundation::Xml.Serialization Engine::Project
           Foundation::Settings Foundation::Scene.Resource
           Pipeline::Cook Foundation::Resource Foundation::Policy
           Pipeline::Core Pipeline::Fonts Pipeline::Texture # the new project's starter font and sky
           Foundation::Shaders Foundation::Shaders.System) # export cooks the engine shader pack (Data/Shaders/shaders.dpak)

# The player executable basename, from the build's single source of truth (root CMakeLists). The
# export host-template stages "<base>[.exe]" and reads "<base>.runtime-libs" beside the running tool.
target_compile_definitions(Editor.Project PRIVATE
    BUILTIN_PLAYER_BASENAME=u8"${BUILTIN_PLAYER_BASENAME}")

# Native ship link (game-native-code.md N3): the exporter re-invokes THIS cmake over THIS
# checkout to build Engine.GamePlayer with the project's native module statically linked.
# Both baked like the shader dir above; a relocated editor reports clearly instead of guessing.
target_compile_definitions(Editor.Project PRIVATE
    BUILDSYSTEM_CMAKE_PATH="${CMAKE_COMMAND}"
    BUILDSYSTEM_ENGINE_ROOT="${CMAKE_SOURCE_DIR}"
    BUILDSYSTEM_CXX_COMPILER="${CMAKE_CXX_COMPILER}")

target_sources(Editor.Project
    PRIVATE
        AllocatorImpl.cpp
        CookServiceImpl.cpp
        ProjectScaffoldImpl.cpp
        ProjectSeedImpl.cpp
        ExportTemplateImpl.cpp
        ExportImpl.cpp
    PUBLIC
        FILE_SET editorproject_modules TYPE CXX_MODULES FILES
            Allocator.cppm
            Project.cppm
            ProjectRegistry.cppm
            ProjectManager.cppm
            Export.cppm
            LogBuffer.cppm
            JobService.cppm
            ExportPreset.cppm
            ExportRoots.cppm
            TemplateIcons.cppm
            ExportTemplate.cppm
            ExportController.cppm
            CookService.cppm
            EditorProjectModule.cppm)
