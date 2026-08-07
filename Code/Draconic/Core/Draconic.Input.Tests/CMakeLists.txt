add_executable(Draconic.Input.Tests
    TestsMain.cpp
    InputTests.cpp)

target_link_libraries(Draconic.Input.Tests PRIVATE
    Draconic::Input Draconic::Xml.Serialization Draconic::Settings
    Draconic::Script Draconic::Script.Wren Draconic::Engine.Input Draconic::Runtime doctest::doctest)

target_compile_definitions(Draconic.Input.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Input.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Input.Tests COMMAND Draconic.Input.Tests)
