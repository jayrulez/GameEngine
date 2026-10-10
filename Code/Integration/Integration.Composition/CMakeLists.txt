# Integration.Composition - the engine composition x the pipeline registration: every factory
# description's cooked form is a registered serializable some builder produces (the link the
# scene format reference joins). Cross-collection (Engine x Pipeline), so it lives here and the
# engine suites stay pipeline-free.
add_executable(Integration.Composition TestsMain.cpp CompositionPipelineTests.cpp UpdateOrderTests.cpp)
target_link_libraries(Integration.Composition PRIVATE
    Engine::Composition Foundation::Resource Pipeline::Registration Pipeline::Core
    Foundation::Core doctest::doctest)
target_compile_definitions(Integration.Composition PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Integration.Composition PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Integration.Composition COMMAND Integration.Composition)
