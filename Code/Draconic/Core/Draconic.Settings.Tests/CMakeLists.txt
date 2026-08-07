add_executable(Draconic.Settings.Tests TestsMain.cpp SettingsTests.cpp)
target_link_libraries(Draconic.Settings.Tests PRIVATE
    Draconic::Settings Draconic::Foundation Draconic::Xml.Serialization Draconic::Xml doctest::doctest)
target_compile_definitions(Draconic.Settings.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Settings.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Settings.Tests COMMAND Draconic.Settings.Tests)
