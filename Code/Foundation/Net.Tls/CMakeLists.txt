# Foundation::Net.Tls - `foundation.net.tls`: the TLS seam (a session as a byte engine, the
# backend interface, the trust a client checks against) and the system's trusted roots. No TLS
# library here: a backend module brings one (Net.Tls.MbedTLS).

util_add_engine_library(Net.Tls ALIAS Foundation::Net.Tls)
target_link_libraries(Net.Tls PUBLIC Foundation::Core Foundation::Policy)

target_sources(Net.Tls
    PRIVATE
        NetTlsImpl.cpp
    PUBLIC
        FILE_SET net_tls_modules TYPE CXX_MODULES FILES
            NetTls.cppm)

# The Windows ROOT store (CertOpenSystemStoreW).
if(WIN32)
    target_link_libraries(Net.Tls PRIVATE crypt32)
endif()
