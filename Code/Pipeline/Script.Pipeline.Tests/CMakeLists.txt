# The library under test (Pipeline::Script) stays backend-neutral and resolves the per-language cook
# through the registry. The neutral text scanners (ScriptPipelineTests.cpp) and reflection
# (ScriptReflectionTests.cpp) always build; each backend's neutral-builder cook round-trip lives in
# its own file, without #ifdefs, compiled only when that backend is on. They share CookBed.h.
set(_script_pipeline_backend_tests "")
set(_script_pipeline_backend_libs "")
if(OPTION_ENABLE_ANGELSCRIPT)
    list(APPEND _script_pipeline_backend_tests AngelScriptPipelineTests.cpp)
    list(APPEND _script_pipeline_backend_libs ${SCRIPTLIB_ANGELSCRIPT_PIPELINE} ${SCRIPTLIB_ANGELSCRIPT})
endif()
if(OPTION_ENABLE_LUAU)
    list(APPEND _script_pipeline_backend_tests LuauPipelineTests.cpp)
    list(APPEND _script_pipeline_backend_libs ${SCRIPTLIB_LUAU_PIPELINE} ${SCRIPTLIB_LUAU})
endif()
add_executable(Script.Pipeline.Tests TestsMain.cpp ScriptPipelineTests.cpp
    ${_script_pipeline_backend_tests} ScriptReflectionTests.cpp)
target_link_libraries(Script.Pipeline.Tests PRIVATE
    Pipeline::Script Foundation::Script.Resource Foundation::Script
    Foundation::Script.Facades ${_script_pipeline_backend_libs}
    Pipeline::Core Editor::Core Foundation::Content Foundation::Resource Foundation::VFS
    doctest::doctest)
target_compile_definitions(Script.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Script.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Script.Pipeline.Tests COMMAND Script.Pipeline.Tests)
