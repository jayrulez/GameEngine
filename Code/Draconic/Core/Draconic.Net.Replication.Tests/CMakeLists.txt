
add_executable(Draconic.Net.Replication.Tests TestsMain.cpp ReplicationTests.cpp)
target_link_libraries(Draconic.Net.Replication.Tests PRIVATE Draconic::Net.Replication Draconic::Scene doctest::doctest)
target_compile_definitions(Draconic.Net.Replication.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Net.Replication.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Net.Replication.Tests COMMAND Draconic.Net.Replication.Tests)
