# ExportTests is mostly backend-neutral (templates/presets/manifests) and always builds so that
# coverage is never gated on a script backend. Its two export-driver tests cook a startup script
# asset, so the file links each enabled backend's cook (guarded) and the script tests themselves are
# #ifdef'd per backend (OPTION_HAS_ANGELSCRIPT / OPTION_HAS_LUAU).
set(_editor_core_script_libs Pipeline::Script Foundation::Script Foundation::Script.Resource)
if(OPTION_ENABLE_ANGELSCRIPT)
    list(APPEND _editor_core_script_libs ${SCRIPTLIB_ANGELSCRIPT_PIPELINE} ${SCRIPTLIB_ANGELSCRIPT})
endif()
if(OPTION_ENABLE_LUAU)
    list(APPEND _editor_core_script_libs ${SCRIPTLIB_LUAU_PIPELINE} ${SCRIPTLIB_LUAU})
endif()
add_executable(Editor.Core.Tests TestsMain.cpp CommandTests.cpp ActionsTests.cpp ProjectTests.cpp CookServiceTests.cpp
    ProjectRegistryTests.cpp
    ExportTests.cpp JobServiceTests.cpp ThumbnailServiceTests.cpp ContextTests.cpp LogBufferTests.cpp
    EditorResourcesTests.cpp)
target_link_libraries(Editor.Core.Tests PRIVATE Editor::Core Pipeline::Geometry Foundation::Geometry.Resource
    Pipeline::Shaders Foundation::Shaders.Resource
    Engine::Render Foundation::Scene.Resource Foundation::VFS.Pak Engine::Project Foundation::Content Foundation::VFS Foundation::Settings
    ${_editor_core_script_libs} doctest::doctest)
target_compile_definitions(Editor.Core.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.Core.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Core.Tests COMMAND Editor.Core.Tests)

target_compile_definitions(Editor.Core.Tests PRIVATE RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
