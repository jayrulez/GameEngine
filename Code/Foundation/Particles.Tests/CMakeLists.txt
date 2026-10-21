# Foundation::Particles unit tests (doctest; pure CPU, no GPU/renderer).

add_executable(Particles.Tests
    TestsMain.cpp
    ParticlesTests.cpp
    ParticleCurveTests.cpp
)

target_link_libraries(Particles.Tests PRIVATE
    Foundation::Xml.Serialization Foundation::Particles doctest::doctest)

target_compile_definitions(Particles.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Particles.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Particles.Tests COMMAND Particles.Tests)
