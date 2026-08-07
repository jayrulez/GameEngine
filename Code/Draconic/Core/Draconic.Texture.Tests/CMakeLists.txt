
add_executable(Draconic.Texture.Tests TestsMain.cpp TextureDataTests.cpp)
target_link_libraries(Draconic.Texture.Tests PRIVATE Draconic::Texture Draconic::RHI Draconic::Image doctest::doctest)
target_compile_definitions(Draconic.Texture.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Texture.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Texture.Tests COMMAND Draconic.Texture.Tests)
