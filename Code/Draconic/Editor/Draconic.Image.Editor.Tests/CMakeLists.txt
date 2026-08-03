
add_executable(DraconicImagePipelineTests TestsMain.cpp ImagePipelineTests.cpp ImageReflectionTests.cpp)
target_link_libraries(DraconicImagePipelineTests PRIVATE
    Draconic::Image.Editor Draconic::Image.Resource Draconic::Image Draconic::Image.IO
    Draconic::Editor Draconic::Content Draconic::Resource Draconic::VFS doctest::doctest)
target_compile_definitions(DraconicImagePipelineTests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(DraconicImagePipelineTests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME DraconicImagePipelineTests COMMAND DraconicImagePipelineTests)
