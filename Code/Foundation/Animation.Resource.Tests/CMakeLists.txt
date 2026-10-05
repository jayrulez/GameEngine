# Foundation::Animation.Resource unit tests (doctest; content DB round-trip - no GPU).

add_executable(Animation.Resource.Tests
    TestsMain.cpp
    AnimationResourceTests.cpp
    RootMotionTests.cpp)

target_link_libraries(Animation.Resource.Tests PRIVATE
    Foundation::Animation.Resource Foundation::VFS Foundation::Xml Foundation::Xml.Serialization doctest::doctest)

target_compile_definitions(Animation.Resource.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Animation.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Animation.Resource.Tests COMMAND Animation.Resource.Tests)
