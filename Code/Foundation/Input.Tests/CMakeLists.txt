add_executable(Input.Tests
    TestsMain.cpp
    InputTests.cpp
    BindingNamesTests.cpp
    ScriptedInputTests.cpp
    InputKindTests.cpp)

target_link_libraries(Input.Tests PRIVATE
    Foundation::Input Foundation::Json Foundation::Shell Foundation::Xml.Serialization Foundation::Settings doctest::doctest)

target_compile_definitions(Input.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Input.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Input.Tests COMMAND Input.Tests)
