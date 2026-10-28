
add_executable(Profiler.UI.Tests TestsMain.cpp ProfilerViewsTests.cpp PanelViewsTests.cpp)
target_link_libraries(Profiler.UI.Tests PRIVATE Foundation::Profiler.UI doctest::doctest)
target_include_directories(Profiler.UI.Tests PRIVATE ${CMAKE_SOURCE_DIR}/Code/Foundation/Profiler.Tests) # KnownSource.h
target_compile_definitions(Profiler.UI.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Profiler.UI.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Profiler.UI.Tests COMMAND Profiler.UI.Tests)
