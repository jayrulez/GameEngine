# Foundation::Net - `foundation.net` (real-time game networking: the wire format, INetTransport
# with swappable backends such as our reliable UDP and the loopback simulator, sessions,
# reliability, RPC, replication). Over Core and its system sockets.

util_add_engine_library(Net ALIAS Foundation::Net)
target_link_libraries(Net PUBLIC Foundation::Core Foundation::Policy)

target_sources(Net
    PUBLIC
        FILE_SET net_modules TYPE CXX_MODULES FILES
            WireStream.cppm
            NetTransport.cppm
            Datagram.cppm
            Reliable.cppm
            NetSession.cppm
            RpcTable.cppm
            UdpSocket.cppm
            TcpSocket.cppm
            NetModule.cppm)

