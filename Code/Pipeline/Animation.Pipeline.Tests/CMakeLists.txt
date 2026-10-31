# Pipeline::Animation unit tests (doctest; a cook-and-load round trip, no GPU).

add_executable(Animation.Pipeline.Tests
    TestsMain.cpp
    AnimationEditorTests.cpp)

target_link_libraries(Animation.Pipeline.Tests PRIVATE
    Pipeline::Animation Foundation::Animation.Resource Foundation::VFS doctest::doctest)

target_compile_definitions(Animation.Pipeline.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Animation.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Animation.Pipeline.Tests COMMAND Animation.Pipeline.Tests)
