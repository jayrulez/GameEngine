# Experimental::GUI driven from scripts (G1 of Specs/vg-and-gui-lab.md): the lab's reflected widgets
# built, styled and clicked from AngelScript and from Luau, and the weak-handle rule in both. Each
# backend leg is #ifdef OPTION_HAS_<backend>-guarded; ${SCRIPTLIB_*} link only the enabled ones.
add_executable(GUI.Script.Tests
    GuiScriptTests.cpp
    TestsMain.cpp)

target_link_libraries(GUI.Script.Tests PRIVATE
    Experimental::GUI Foundation::Core Foundation::Script ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU}
    doctest::doctest)

target_compile_definitions(GUI.Script.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(GUI.Script.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME GUI.Script.Tests COMMAND GUI.Script.Tests)
