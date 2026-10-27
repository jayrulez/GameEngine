add_executable(Profiler.GUI.Tests TestsMain.cpp ProfileFocusTests.cpp FrameStripTests.cpp ProfileTimelineTests.cpp)
target_link_libraries(Profiler.GUI.Tests PRIVATE Foundation::Profiler.GUI Foundation::GUI doctest::doctest)
target_include_directories(Profiler.GUI.Tests PRIVATE ${CMAKE_SOURCE_DIR}/Code/Foundation/Profiler.Tests) # KnownSource.h
target_compile_definitions(Profiler.GUI.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Profiler.GUI.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Profiler.GUI.Tests COMMAND Profiler.GUI.Tests)
