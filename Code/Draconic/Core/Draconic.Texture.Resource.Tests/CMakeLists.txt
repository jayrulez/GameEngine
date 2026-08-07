
add_executable(DraconicTextureFactoryTests TestsMain.cpp TextureFactoryTests.cpp)
target_link_libraries(DraconicTextureFactoryTests PRIVATE
    Draconic::Texture.Resource Draconic::Texture Draconic::RHI Draconic::RHI.Null
    Draconic::Content Draconic::Resource Draconic::VFS doctest::doctest)
target_compile_definitions(DraconicTextureFactoryTests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(DraconicTextureFactoryTests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME DraconicTextureFactoryTests COMMAND DraconicTextureFactoryTests)
