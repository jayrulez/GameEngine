
# Backend-neutral: the script-facade acceptance test lives in Integration.ScriptFacades.
add_executable(Net.Manager.Tests TestsMain.cpp ManagerTests.cpp ReplicationWiringTests.cpp WebSocketJoinTests.cpp)
target_link_libraries(Net.Manager.Tests PRIVATE Foundation::Net.Manager Foundation::Net.Tls.MbedTLS doctest::doctest)
target_compile_definitions(Net.Manager.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Net.Manager.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Net.Manager.Tests COMMAND Net.Manager.Tests)
