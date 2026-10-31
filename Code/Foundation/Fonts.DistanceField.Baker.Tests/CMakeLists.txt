# Foundation::Fonts.DistanceField.Baker tests (doctest): bakes real glyphs and inspects MSDF cell placement.

add_executable(Fonts.DistanceField.Baker.Tests TestsMain.cpp DistanceFieldBakerTests.cpp)
target_link_libraries(Fonts.DistanceField.Baker.Tests PRIVATE Foundation::VFS
    Foundation::Fonts.DistanceField.Baker Foundation::Fonts.DistanceField Foundation::Fonts Foundation::Fonts.TrueType
    doctest::doctest)
target_compile_definitions(Fonts.DistanceField.Baker.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Fonts.DistanceField.Baker.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Fonts.DistanceField.Baker.Tests COMMAND Fonts.DistanceField.Baker.Tests)
