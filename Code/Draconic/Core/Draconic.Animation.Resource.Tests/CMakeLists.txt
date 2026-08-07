# Draconic::Animation.Resource unit tests (doctest; content DB round-trip - no GPU).

add_executable(Draconic.Animation.Resource.Tests
    TestsMain.cpp
    AnimationResourceTests.cpp)

target_link_libraries(Draconic.Animation.Resource.Tests PRIVATE
    Draconic::Animation.Resource Draconic::VFS doctest::doctest)

target_compile_definitions(Draconic.Animation.Resource.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Animation.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Animation.Resource.Tests COMMAND Draconic.Animation.Resource.Tests)
