add_executable(Draconic.Audio.Tests
    TestsMain.cpp
    AudioEngineTests.cpp)

target_link_libraries(Draconic.Audio.Tests PRIVATE
    Draconic::Audio doctest::doctest)

target_compile_definitions(Draconic.Audio.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Audio.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Audio.Tests COMMAND Draconic.Audio.Tests)
