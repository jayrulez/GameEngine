
add_executable(Draconic.Xml.Tests TestsMain.cpp LexerTests.cpp NodeTests.cpp NamespaceTests.cpp ParserTests.cpp WriterTests.cpp)
target_link_libraries(Draconic.Xml.Tests PRIVATE Draconic::Xml doctest::doctest)
target_compile_definitions(Draconic.Xml.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Xml.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Xml.Tests COMMAND Draconic.Xml.Tests)
