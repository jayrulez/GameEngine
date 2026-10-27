add_executable(Particles.Pipeline.Tests TestsMain.cpp ParticlePipelineTests.cpp)
target_link_libraries(Particles.Pipeline.Tests PRIVATE
    Pipeline::Particles Foundation::Particles.Resource Foundation::Particles
    Foundation::Content Foundation::Resource Pipeline::Core Foundation::VFS
    Foundation::Texture Foundation::Texture.Resource Foundation::RHI Foundation::RHI.Null Foundation::Xml.Serialization
    doctest::doctest)
target_compile_definitions(Particles.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the version 0 fixture
target_compile_options(Particles.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Particles.Pipeline.Tests COMMAND Particles.Pipeline.Tests)
