# Experimental::GUI.Gamekit's tests: a game's screens (K4 of Specs/gui-lab-evolution-3.md), from
# code, markup and scripts. Each script backend leg is #ifdef OPTION_HAS_<backend>-guarded.
add_executable(GUI.Gamekit.Tests
    ScreenStackTests.cpp
    TestsMain.cpp)

target_link_libraries(GUI.Gamekit.Tests PRIVATE
    Experimental::GUI.Gamekit Experimental::GUI Foundation::Core Foundation::VG.SVG Foundation::Script ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU}
    doctest::doctest)

target_compile_definitions(GUI.Gamekit.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(GUI.Gamekit.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME GUI.Gamekit.Tests COMMAND GUI.Gamekit.Tests)
