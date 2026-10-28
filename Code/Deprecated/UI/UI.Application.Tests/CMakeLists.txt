add_executable(UI.Application.Tests
    UIApplicationTests.cpp
    TestsMain.cpp)

target_link_libraries(UI.Application.Tests PRIVATE
    Foundation::UI.Application Foundation::UI.Toolkit Foundation::Shell Foundation::Core
    doctest::doctest)

target_compile_definitions(UI.Application.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(UI.Application.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME UI.Application.Tests COMMAND UI.Application.Tests)
