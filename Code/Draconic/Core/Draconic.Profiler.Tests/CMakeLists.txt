
add_executable(Draconic.Profiler.Tests TestsMain.cpp ProfilerTests.cpp)
target_link_libraries(Draconic.Profiler.Tests PRIVATE Draconic::Profiler doctest::doctest)
target_compile_definitions(Draconic.Profiler.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Profiler.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Profiler.Tests COMMAND Draconic.Profiler.Tests)
