
add_executable(DraconicTexturePipelineTests TestsMain.cpp TexturePipelineTests.cpp TextureReflectionTests.cpp)
target_link_libraries(DraconicTexturePipelineTests PRIVATE
    Draconic::Texture.Editor Draconic::Texture.Resource Draconic::Texture Draconic::Editor.Core
    Draconic::RHI Draconic::RHI.Null Draconic::Editor Draconic::Image Draconic::Image.IO
    Draconic::Content Draconic::Resource Draconic::VFS doctest::doctest)
target_compile_definitions(DraconicTexturePipelineTests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(DraconicTexturePipelineTests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME DraconicTexturePipelineTests COMMAND DraconicTexturePipelineTests)
