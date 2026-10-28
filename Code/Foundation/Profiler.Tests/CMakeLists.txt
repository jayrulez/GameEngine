
add_executable(Profiler.Tests TestsMain.cpp ProfilerTests.cpp ProfileStreamTests.cpp ProfileRecordingTests.cpp ProfileWindowTests.cpp)
target_link_libraries(Profiler.Tests PRIVATE Foundation::Profiler doctest::doctest)
target_compile_definitions(Profiler.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Profiler.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Profiler.Tests COMMAND Profiler.Tests)
