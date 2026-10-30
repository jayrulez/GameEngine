add_executable(Http.Client.Tests TestsMain.cpp HttpClientTests.cpp)
target_link_libraries(Http.Client.Tests PRIVATE
    Foundation::Http.Client Foundation::Http Foundation::Net.Tls.MbedTLS Foundation::Net.Tls Foundation::Net Foundation::Core doctest::doctest)
target_compile_definitions(Http.Client.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Http.Client.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Http.Client.Tests COMMAND Http.Client.Tests)

# The browser client, checked by hand (a wasm build runs no tests): a page that sends a few requests
# through fetch and prints the answers. Serve its folder and open Http.Client.WebCheck.html.
if(EMSCRIPTEN)
    add_executable(Http.Client.WebCheck WebCheck.cpp)
    target_link_libraries(Http.Client.WebCheck PRIVATE Foundation::Http.Client Foundation::Http Foundation::Core)
    target_link_options(Http.Client.WebCheck PRIVATE "-sFETCH=1")
    set_target_properties(Http.Client.WebCheck PROPERTIES SUFFIX ".html")
endif()
