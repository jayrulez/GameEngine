add_executable(Draconic.Particles.Resource.Tests TestsMain.cpp ParticleEffectFactoryTests.cpp)
target_link_libraries(Draconic.Particles.Resource.Tests PRIVATE
    Draconic::Particles.Resource Draconic::Particles
    Draconic::Content Draconic::Resource Draconic::VFS doctest::doctest)
target_compile_definitions(Draconic.Particles.Resource.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Particles.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Particles.Resource.Tests COMMAND Draconic.Particles.Resource.Tests)
