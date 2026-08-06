
add_executable(Draconic.Engine.GameInstance.Tests TestsMain.cpp GameInstanceTests.cpp)
target_link_libraries(Draconic.Engine.GameInstance.Tests PRIVATE
    Draconic::Engine.GameInstance Draconic::Net Draconic::Net.Manager
    Draconic::Input Draconic::Shell Draconic::Script.Wren Draconic::Script.AngelScript
    Draconic::Scene.Resource Draconic::Content Draconic::Resource Draconic::VFS
    doctest::doctest)
target_compile_definitions(Draconic.Engine.GameInstance.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Engine.GameInstance.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Engine.GameInstance.Tests COMMAND Draconic.Engine.GameInstance.Tests)
