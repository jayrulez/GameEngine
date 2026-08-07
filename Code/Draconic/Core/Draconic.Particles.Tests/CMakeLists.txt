# Draconic::Particles unit tests (doctest; pure CPU, no GPU/renderer).

add_executable(Draconic.Particles.Tests
    TestsMain.cpp
    ParticlesTests.cpp
    ResourceRefTests.cpp)

target_link_libraries(Draconic.Particles.Tests PRIVATE
    Draconic::Particles Draconic::Engine.Particles Draconic::Particles.Resource
    Draconic::Scene.Resource Draconic::Resource Draconic::VFS Draconic::Content doctest::doctest)

target_compile_definitions(Draconic.Particles.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Particles.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Particles.Tests COMMAND Draconic.Particles.Tests)
