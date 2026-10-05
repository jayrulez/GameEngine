# Foundation::Animation unit tests (doctest; pure logic over Core math).

add_executable(Animation.Tests
    TestsMain.cpp
    EasingTypeTests.cpp
    PoseTests.cpp
    SkeletonTests.cpp
    ClipTests.cpp
    SamplerTests.cpp
    PlayerTests.cpp
    GraphTests.cpp
    ModifierTests.cpp
    IkTests.cpp
    RootMotionTests.cpp)

target_link_libraries(Animation.Tests PRIVATE
    Foundation::Animation doctest::doctest)

target_compile_definitions(Animation.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Animation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Animation.Tests COMMAND Animation.Tests)
