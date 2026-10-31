# Texture.Compression.Tests - the policy table, exact block sizes, and the PSNR of an encode and
# decode round trip. Links ThirdParty::Bc7enc directly for the decode side (the module only
# encodes; decoding is test-only), the one place outside Texture.Compression that touches the
# encoder headers.
add_executable(Texture.Compression.Tests TestsMain.cpp CompressionTests.cpp)
target_link_libraries(Texture.Compression.Tests PRIVATE ThirdParty::Bcdec
    Pipeline::Texture.Compression Foundation::Core Foundation::RHI
    ThirdParty::Bc7enc ThirdParty::Astcenc doctest::doctest)
target_compile_definitions(Texture.Compression.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
# The vendored bc7enc/rgbcx decode headers pull in the deprecated <ciso646> (a #warning under C++20).
target_compile_options(Texture.Compression.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Texture.Compression.Tests COMMAND Texture.Compression.Tests)
