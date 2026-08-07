# Draconic::Net - `draconic.net` (real-time game networking: wire format + INetTransport with
# swappable backends - reliable-UDP (ours), loopback/sim, later websocket/webrtc - + session,
# reliability, RPC, replication). Over Foundation (+ Foundation/System sockets, later). See
# docs/design/networking.md.

add_library(Draconic.Net STATIC)
add_library(Draconic::Net ALIAS Draconic.Net)

target_link_libraries(Draconic.Net PUBLIC Draconic::Foundation Draconic::Policy)

target_sources(Draconic.Net
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

