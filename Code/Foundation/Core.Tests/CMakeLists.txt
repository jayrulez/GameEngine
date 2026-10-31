# Foundation::Core unit tests (doctest).
#
# doctest is configured for -fno-exceptions (DOCTEST_CONFIG_NO_EXCEPTIONS_*),
# matching the engine-wide policy that Foundation::Core propagates from Foundation::Policy.

# doctest (the test framework) is vendored in ThirdParty/ as doctest::doctest.

# One file per subsystem; TestsMain.cpp is the only TU that emits main.
add_executable(Core.Tests
    TestsMain.cpp
    BaseTests.cpp
    DebugTests.cpp
    MemoryTests.cpp
    SystemTests.cpp
    TimeTests.cpp
    ContainersTests.cpp
    SimdMathTests.cpp
    ContentFitTests.cpp
    StringTests.cpp
    StringUtilTests.cpp
    FormatLogTests.cpp
    MathTests.cpp
    CurveTests.cpp
    GradientTests.cpp
    RandomTests.cpp
    GuidTests.cpp
    RttiTests.cpp
    CoreReflectionTests.cpp
    IoTests.cpp
    VersionedSerializationTests.cpp
    FramedRegionTests.cpp
    FunctionTests.cpp
    ThreadingTests.cpp
    LibraryTests.cpp
    AllocatorTripwireTests.cpp
    SharedLibTripwireTests.cpp
    SchemaRecorderTests.cpp)

target_link_libraries(Core.Tests PRIVATE Foundation::Core doctest::doctest)
# The allocator tripwire scans the source tree at test time.
target_compile_definitions(Core.Tests
                           PRIVATE RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# A real shared library for the DynamicLibrary tests to load at runtime.
add_library(TestPlugin MODULE TestPlugin.cpp)
set_target_properties(TestPlugin PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_dependencies(Core.Tests TestPlugin)

# DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN is set in TestsMain.cpp only (not here),
# so the other TUs don't each emit a main.
target_compile_definitions(Core.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    TEST_PLUGIN_PATH="$<TARGET_FILE:TestPlugin>")

# doctest 2.4.11 includes <ciso646>, which emits a deprecation #warning under
# C++23. It is third-party noise, so only that warning stops being an error for this target;
# our own code keeps -Werror.
target_compile_options(Core.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Core.Tests COMMAND Core.Tests)
