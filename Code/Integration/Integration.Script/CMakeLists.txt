# Integration.Script - scripting backends exercising reflected engine/foundation types end-to-end
# (JSON parse/build/query/stringify from AngelScript and Luau). It crosses collections
# (foundation.json and foundation.script.<backend>), so it lives in Code/Integration, not Json.Tests.

add_executable(Integration.Script TestsMain.cpp JsonScriptTests.cpp)
target_link_libraries(Integration.Script PRIVATE
    Foundation::Json Foundation::Script doctest::doctest)

# Each backend is independently switchable; include its leg only when the backend is built.
if(TARGET ${SCRIPTLIB_ANGELSCRIPT})
    target_link_libraries(Integration.Script PRIVATE ${SCRIPTLIB_ANGELSCRIPT})
    target_compile_definitions(Integration.Script PRIVATE INTEGRATION_HAS_ANGELSCRIPT=1)
endif()
if(TARGET ${SCRIPTLIB_LUAU})
    target_link_libraries(Integration.Script PRIVATE ${SCRIPTLIB_LUAU})
    target_compile_definitions(Integration.Script PRIVATE INTEGRATION_HAS_LUAU=1)
endif()

target_compile_definitions(Integration.Script PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Integration.Script PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Integration.Script COMMAND Integration.Script)
