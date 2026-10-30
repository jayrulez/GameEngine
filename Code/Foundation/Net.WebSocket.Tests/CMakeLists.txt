add_executable(Net.WebSocket.Tests TestsMain.cpp WebSocketTests.cpp WssTests.cpp)
target_link_libraries(Net.WebSocket.Tests PRIVATE Foundation::Net.WebSocket Foundation::Net.Tls.MbedTLS Foundation::Net.Tls Foundation::Net Foundation::Http doctest::doctest)
target_compile_definitions(Net.WebSocket.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Net.WebSocket.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Net.WebSocket.Tests COMMAND Net.WebSocket.Tests)
