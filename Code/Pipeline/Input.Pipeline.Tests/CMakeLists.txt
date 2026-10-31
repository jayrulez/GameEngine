# input.pipeline tests: InputMapAsset reflection (the Nested map and the traversable InputMap tree).
add_executable(Input.Pipeline.Tests TestsMain.cpp InputMapAssetTests.cpp)
target_link_libraries(Input.Pipeline.Tests PRIVATE
    Pipeline::Input Foundation::Input Pipeline::Core doctest::doctest)
target_compile_definitions(Input.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Input.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Input.Pipeline.Tests COMMAND Input.Pipeline.Tests)
