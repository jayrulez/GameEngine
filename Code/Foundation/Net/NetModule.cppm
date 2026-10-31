// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Net - the `foundation.net` module.
///
/// Real-time game networking: the wire format, the datagram socket interface (IDatagramSocket:
/// UDP, or the in-memory sim), a transport abstraction (INetTransport) with a reliable-UDP backend
/// and an in-memory loopback for deterministic tests, and the session and RPC layers on top. It
/// sits over the Core/System socket primitives. Replication, WebSocket and HTTP are separate
/// sibling modules (foundation.net.replication, foundation.net.websocket, foundation.http).
///
/// The wire layer, the transport interface and the loopback and sim transports are all testable
/// headless with no OS sockets.

export module foundation.net;

export import :wire;
export import :transport;
export import :datagram;
export import :reliable;
export import :session;
export import :rpc;
export import :udp_socket;
export import :tcp_socket;
