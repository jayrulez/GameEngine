# Draconic::Editor.Core - `draconic.editor.core` (headless editor domain: context, registries,
# command stack, pages, selection, project). Tooling only - the runtime never links this.

add_library(Draconic.Editor.Core STATIC)
add_library(Draconic::Editor.Core ALIAS Draconic.Editor.Core)

target_link_libraries(Draconic.Editor.Core
    PUBLIC Draconic::Foundation Draconic::VFS Draconic::VFS.Pak Draconic::Content Draconic::Xml.Serialization Draconic::Engine.Project
           Draconic::Settings Draconic::Scene.Resource
           Draconic::Editor Draconic::Editor.Cook Draconic::Resource Draconic::Policy
           Draconic::Shaders) # export cooks the engine shader pack (shaders.dpak)

# Export cooks the built-in shaders from the source tree (same root the renderer bakes); a
# relocated editor falls back to a "Shaders" dir beside it.
if(TARGET Draconic::Shaders)
    target_compile_definitions(Draconic.Editor.Core PRIVATE
        DRACONIC_ENGINE_SHADER_DIR="${CMAKE_SOURCE_DIR}/Data/Shaders")
endif()

# The player executable basename, from the build's single source of truth (root CMakeLists). The
# export host-template stages "<base>[.exe]" and reads "<base>.runtime-libs" beside the running tool.
target_compile_definitions(Draconic.Editor.Core PRIVATE
    DRACONIC_PLAYER_BASENAME=u8"${DRACONIC_PLAYER_BASENAME}")

target_sources(Draconic.Editor.Core
    PRIVATE
        CookServiceImpl.cpp
        ContextImpl.cpp
        CommandImpl.cpp
        ImporterImpl.cpp
        ExportTemplateImpl.cpp
        ExportImpl.cpp
    PUBLIC
        FILE_SET editorcore_modules TYPE CXX_MODULES FILES
            Command.cppm
            Selection.cppm
            Project.cppm
            ProjectRegistry.cppm
            ProjectManager.cppm
            Export.cppm
            Page.cppm
            Context.cppm
            LogBuffer.cppm
            JobService.cppm
            ExportPreset.cppm
            EditorSettings.cppm
            ExportRoots.cppm
            ExportTemplate.cppm
            ExportController.cppm
            CookService.cppm
            Importer.cppm
            EditorCoreModule.cppm)

