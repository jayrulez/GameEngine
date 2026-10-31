# Foundation::Shell.Desktop unit tests (doctest). Force SDL's dummy video
# driver at runtime so these are headless/CI-safe.

add_executable(Shell.Desktop.Tests
    TestsMain.cpp
    SDL3ShellTests.cpp)

# The test TU includes <SDL3/SDL.h> directly to push synthetic events.
target_link_libraries(Shell.Desktop.Tests PRIVATE
    Foundation::Shell.Desktop Foundation::Runtime.Client Foundation::Runtime.Desktop ${ENGINE_SDL_TARGET} doctest::doctest)

target_compile_definitions(Shell.Desktop.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Shell.Desktop.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Shell.Desktop.Tests COMMAND Shell.Desktop.Tests)

# Copy SDL3.dll next to the test executable on Windows. Nothing to stage in a static
# engine build (SDL is a vendored archive there). Under ENGINE_SHARED_LIBS SDL is shared, one
# runtime for this exe and Shell.Desktop.dll, so the events the tests push reach the shell, and
# TARGET_RUNTIME_DLLS picks it up.
util_copy_runtime_deps(Shell.Desktop.Tests)
