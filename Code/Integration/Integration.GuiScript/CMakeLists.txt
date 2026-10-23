# Integration.GuiScript - the experimental GUI and its satellites driven from the script backends
# (the lab's reflected widgets built, styled, bound and clicked, and the gamekit's screen stack,
# from AngelScript + Luau). Cross-layer (scripting x a GUI library), so it lives here and the
# libraries' own suites stay backend-neutral. Apart from
# Integration.ScriptFacades: the lab and foundation.ui both reflect a Label, a Button and a
# ScreenStack, and one process binds only one of them to its scripts. Each backend leg is
# #ifdef OPTION_HAS_<backend>-guarded; link only the enabled backends via ${SCRIPTLIB_*}.
add_executable(Integration.GuiScript TestsMain.cpp GuiScriptTests.cpp GamekitScriptTests.cpp)
target_link_libraries(Integration.GuiScript PRIVATE
    Experimental::GUI.Gamekit Experimental::GUI Foundation::Core Foundation::Script
    ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU}
    doctest::doctest)
target_compile_definitions(Integration.GuiScript PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Integration.GuiScript PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Integration.GuiScript COMMAND Integration.GuiScript)
