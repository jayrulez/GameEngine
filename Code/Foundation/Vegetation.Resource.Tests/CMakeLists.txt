# Foundation::Vegetation.Resource unit tests (doctest; the mask raster, its brush cores and the
# cooked source round trip; no GPU).

add_executable(Vegetation.Resource.Tests TestsMain.cpp VegetationMaskTests.cpp)
target_link_libraries(Vegetation.Resource.Tests PRIVATE Foundation::Vegetation.Resource
    Foundation::Resource Foundation::Content doctest::doctest)
target_compile_definitions(Vegetation.Resource.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Vegetation.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Vegetation.Resource.Tests COMMAND Vegetation.Resource.Tests)
