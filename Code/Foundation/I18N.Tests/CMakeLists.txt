# Foundation::I18N unit tests (doctest; headless).

add_executable(I18N.Tests TestsMain.cpp LocaleTests.cpp PluralRuleTests.cpp StringTableTests.cpp LocalizationTests.cpp)
target_link_libraries(I18N.Tests PRIVATE Foundation::I18N doctest::doctest)
target_compile_definitions(I18N.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(I18N.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME I18N.Tests COMMAND I18N.Tests)
