/// Draconic::Net - the `draconic.net` module.
///
/// Real-time game networking (docs/design/networking.md): the wire format, a transport abstraction
/// (INetTransport) with swappable backends - reliable-UDP (ours), an in-memory loopback/sim for
/// deterministic tests, and later websocket/webrtc - plus session, reliability, RPC, and
/// replication built on top. Sits over the Foundation/System socket primitives; HTTP is a separate
/// sibling module (draconic.http), not part of this one.
///
/// P0 (this slice): the wire layer + the transport seam + the loopback/sim transport, all headlessly
/// testable with no OS sockets.

export module draconic.net;

export import :wire;
export import :transport;
export import :datagram;
export import :reliable;
export import :session;
export import :rpc;
export import :udp_socket;
export import :tcp_socket;
