add_executable(Draconic.Image.Tests TestsMain.cpp ImageTests.cpp ImageDataTests.cpp ImageManipulationTests.cpp NormalMapTests.cpp)
target_link_libraries(Draconic.Image.Tests PRIVATE Draconic::Image.IO Draconic::Image doctest::doctest)
target_compile_definitions(Draconic.Image.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Image.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Image.Tests COMMAND Draconic.Image.Tests)
