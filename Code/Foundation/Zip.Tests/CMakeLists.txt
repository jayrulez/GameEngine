add_executable(Zip.Tests TestsMain.cpp ZipTests.cpp)
target_link_libraries(Zip.Tests PRIVATE Foundation::Zip Foundation::Core doctest::doctest)
target_compile_definitions(Zip.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(Zip.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Zip.Tests COMMAND Zip.Tests)
