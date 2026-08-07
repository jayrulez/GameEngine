
add_executable(Draconic.Net.Tests TestsMain.cpp WireTests.cpp TransportTests.cpp DatagramTests.cpp ReliableTests.cpp SessionTests.cpp RpcTests.cpp UdpSocketTests.cpp TcpSocketTests.cpp)
target_link_libraries(Draconic.Net.Tests PRIVATE Draconic::Net doctest::doctest)
target_compile_definitions(Draconic.Net.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Net.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Net.Tests COMMAND Draconic.Net.Tests)
