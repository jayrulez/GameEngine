# Foundation::Net.Tls.MbedTLS - `foundation.net.tls.mbedtls`: the Mbed TLS backend of the TLS
# interfaces, and a self-signed certificate maker. The only module that links ThirdParty::MbedTLS
# (privately, so its headers stay in the implementation unit).

util_add_engine_library(Net.Tls.MbedTLS ALIAS Foundation::Net.Tls.MbedTLS)
target_link_libraries(Net.Tls.MbedTLS
    PUBLIC Foundation::Core Foundation::Net.Tls Foundation::Policy
    PRIVATE ThirdParty::MbedTLS)

target_sources(Net.Tls.MbedTLS
    PRIVATE
        MbedTlsBackendImpl.cpp
    PUBLIC
        FILE_SET net_tls_mbedtls_modules TYPE CXX_MODULES FILES
            MbedTlsBackend.cppm)
