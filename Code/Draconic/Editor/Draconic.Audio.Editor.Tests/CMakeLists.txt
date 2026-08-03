add_executable(DraconicAudioPipelineTests TestsMain.cpp AudioPipelineTests.cpp AudioReflectionTests.cpp)
target_link_libraries(DraconicAudioPipelineTests PRIVATE
    Draconic::Audio.Editor Draconic::Audio.Resource Draconic::Audio Draconic::Editor
    Draconic::Editor.Core Draconic::Content Draconic::Resource Draconic::VFS doctest::doctest)
target_compile_definitions(DraconicAudioPipelineTests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(DraconicAudioPipelineTests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME DraconicAudioPipelineTests COMMAND DraconicAudioPipelineTests)
