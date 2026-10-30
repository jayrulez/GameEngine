add_executable(Net.Tls.MbedTLS.Tests TestsMain.cpp TlsTests.cpp)
target_link_libraries(Net.Tls.MbedTLS.Tests PRIVATE Foundation::Net.Tls.MbedTLS Foundation::Net.Tls doctest::doctest)
target_compile_definitions(Net.Tls.MbedTLS.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Net.Tls.MbedTLS.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Net.Tls.MbedTLS.Tests COMMAND Net.Tls.MbedTLS.Tests)
