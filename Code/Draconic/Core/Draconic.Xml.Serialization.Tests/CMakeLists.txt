
add_executable(Draconic.Xml.Serialization.Tests TestsMain.cpp XmlSerializerTests.cpp)
target_link_libraries(Draconic.Xml.Serialization.Tests PRIVATE Draconic::Xml.Serialization Draconic::Xml doctest::doctest)
target_compile_definitions(Draconic.Xml.Serialization.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Xml.Serialization.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Xml.Serialization.Tests COMMAND Draconic.Xml.Serialization.Tests)
