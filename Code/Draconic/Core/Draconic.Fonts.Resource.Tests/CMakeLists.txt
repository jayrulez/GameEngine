add_executable(Draconic.Fonts.Resource.Tests TestsMain.cpp FontResourceTests.cpp)
target_link_libraries(Draconic.Fonts.Resource.Tests PRIVATE
    Draconic::Fonts.Resource Draconic::Fonts Draconic::Fonts.Baked Draconic::Fonts.DistanceField
    Draconic::Image Draconic::Content Draconic::Resource Draconic::VFS doctest::doctest)
target_compile_definitions(Draconic.Fonts.Resource.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Fonts.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Fonts.Resource.Tests COMMAND Draconic.Fonts.Resource.Tests)
