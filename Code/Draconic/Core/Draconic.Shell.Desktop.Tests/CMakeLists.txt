# Draconic::Shell.Desktop unit tests (doctest). Force SDL's dummy video
# driver at runtime so these are headless/CI-safe.

add_executable(Draconic.Shell.Desktop.Tests
    TestsMain.cpp
    SDL3ShellTests.cpp)

# The test TU includes <SDL3/SDL.h> directly to push synthetic events.
target_link_libraries(Draconic.Shell.Desktop.Tests PRIVATE
    Draconic::Shell.Desktop Draconic::Runtime.Client Draconic::Runtime.Desktop SDL3::SDL3 doctest::doctest)

target_compile_definitions(Draconic.Shell.Desktop.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Shell.Desktop.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Shell.Desktop.Tests COMMAND Draconic.Shell.Desktop.Tests)

# Copy SDL3.dll next to the test executable on Windows.
draconic_copy_runtime_deps(Draconic.Shell.Desktop.Tests EXTRA ${DRACONIC_SDL3_DLL})
