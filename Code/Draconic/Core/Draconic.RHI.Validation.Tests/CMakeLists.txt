# Draconic::RHI.Validation unit tests (doctest). Wraps the Null backend.

add_executable(Draconic.RHI.Validation.Tests
    TestsMain.cpp
    ValidationTests.cpp)

target_link_libraries(Draconic.RHI.Validation.Tests PRIVATE
    Draconic::RHI.Validation Draconic::RHI.Null doctest::doctest)

target_compile_definitions(Draconic.RHI.Validation.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.RHI.Validation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.RHI.Validation.Tests COMMAND Draconic.RHI.Validation.Tests)
