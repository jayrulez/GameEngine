
add_executable(Draconic.Net.Manager.Tests TestsMain.cpp ManagerTests.cpp FacadeScriptTests.cpp ReplicationWiringTests.cpp)
target_link_libraries(Draconic.Net.Manager.Tests PRIVATE Draconic::Net.Manager
    Draconic::Script.Wren Draconic::Script.AngelScript doctest::doctest)
target_compile_definitions(Draconic.Net.Manager.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Net.Manager.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Net.Manager.Tests COMMAND Draconic.Net.Manager.Tests)
