# Draconic::Animation unit tests (doctest; pure logic over Foundation math).

add_executable(Draconic.Animation.Tests
    TestsMain.cpp
    EasingTypeTests.cpp
    PoseTests.cpp
    SkeletonTests.cpp
    ClipTests.cpp
    SamplerTests.cpp
    PlayerTests.cpp
    GraphTests.cpp)

target_link_libraries(Draconic.Animation.Tests PRIVATE
    Draconic::Animation doctest::doctest)

target_compile_definitions(Draconic.Animation.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Animation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Animation.Tests COMMAND Draconic.Animation.Tests)
