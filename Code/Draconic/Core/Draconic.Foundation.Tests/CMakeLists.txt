# Draconic::Foundation unit tests (doctest).
#
# doctest is configured for -fno-exceptions (DOCTEST_CONFIG_NO_EXCEPTIONS_*),
# matching the engine-wide policy propagated via Draconic::Foundation -> Draconic::Policy.

# doctest (the test framework) is vendored in ThirdParty/ as doctest::doctest.

# One file per subsystem; TestsMain.cpp is the only TU that emits main.
add_executable(Draconic.Foundation.Tests
    TestsMain.cpp
    BaseTests.cpp
    DebugTests.cpp
    MemoryTests.cpp
    SystemTests.cpp
    TimeTests.cpp
    ContainersTests.cpp
    SimdMathTests.cpp
    StringTests.cpp
    StringUtilTests.cpp
    FormatLogTests.cpp
    MathTests.cpp
    RandomTests.cpp
    GuidTests.cpp
    RttiTests.cpp
    FoundationReflectionTests.cpp
    IoTests.cpp
    VersionedSerializationTests.cpp
    FramedRegionTests.cpp
    FunctionTests.cpp
    ThreadingTests.cpp
    LibraryTests.cpp)

target_link_libraries(Draconic.Foundation.Tests PRIVATE Draconic::Foundation doctest::doctest)

# A real shared library for the DynamicLibrary tests to load at runtime.
add_library(DraconicTestPlugin MODULE TestPlugin.cpp)
set_target_properties(DraconicTestPlugin PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_dependencies(Draconic.Foundation.Tests DraconicTestPlugin)

# DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN is set in TestsMain.cpp only (not here),
# so the other TUs don't each emit a main.
target_compile_definitions(Draconic.Foundation.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    DRACONIC_TEST_PLUGIN_PATH="$<TARGET_FILE:DraconicTestPlugin>")

# doctest 2.4.11 includes <ciso646>, which emits a deprecation #warning under
# C++23. It's third-party noise - de-promote just that warning for this target;
# our own code keeps -Werror.
target_compile_options(Draconic.Foundation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Foundation.Tests COMMAND Draconic.Foundation.Tests)
