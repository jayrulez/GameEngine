# Foundation::Model.FBX unit tests (doctest). ufbx also reads OBJ, so the documents are
# written as OBJ (with .mtl) into the test scratch dir; no binary fixtures are needed.

add_executable(Model.FBX.Tests TestsMain.cpp FbxLoaderTests.cpp)
target_link_libraries(Model.FBX.Tests PRIVATE Foundation::Model.FBX Foundation::Model doctest::doctest)
target_compile_definitions(Model.FBX.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Model.FBX.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Model.FBX.Tests COMMAND Model.FBX.Tests)
