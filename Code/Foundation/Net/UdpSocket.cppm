// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Net - `foundation.net:udp_socket` partition.
///
/// The real UDP IDatagramSocket backend: wraps the Core/System UDP primitives (sockets live in
/// Core/System) so ReliableTransport, proven against the deterministic sim, runs over an actual
/// network with zero protocol changes. IPv4 only; a DatagramEndpoint packs (ip << 16) | port in
/// host order.

module;
#include "Core/Prelude.h"

export module foundation.net:udp_socket;

import foundation.core;
import :datagram;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::net
{

    // Pack/unpack an IPv4 DatagramEndpoint (value = (ip << 16) | port, host order).
    [[nodiscard]] inline DatagramEndpoint MakeEndpoint(u32 ip, u16 port) noexcept
    {
        return DatagramEndpoint{(static_cast<u64>(ip) << 16) | static_cast<u64>(port)};
    }
    [[nodiscard]] inline u32 EndpointIp(const DatagramEndpoint& e) noexcept
    {
        return static_cast<u32>(e.value >> 16);
    }
    [[nodiscard]] inline u16 EndpointPort(const DatagramEndpoint& e) noexcept
    {
        return static_cast<u16>(e.value & 0xFFFFull);
    }

    /// The endpoint for `host` (a dotted-quad literal or a DNS name; a name resolves through
    /// the platform resolver and blocks for that lookup, so call it when connecting) and
    /// `port`. Invalid (IsValid() false) when the host does not resolve to an IPv4 address.
    [[nodiscard]] inline DatagramEndpoint ResolveEndpoint(StringView host, u16 port)
    {
        u32 ip = 0;
        if (!core::ResolveHostIPv4(host, ip))
        {
            return DatagramEndpoint{};
        }
        return MakeEndpoint(ip, port);
    }

    // A real UDP socket presented as an IDatagramSocket. Non-blocking; owns one Core/System handle and
    // a refcount on the platform networking layer.
    class UdpSocket final : public IDatagramSocket
    {
    public:
        // Bind to `port` (0 = OS-assigned). Check IsOpen() afterwards.
        explicit UdpSocket(u16 port = 0)
        {
            core::InitializeNetworking();
            m_handle = core::UdpOpen(port, &m_boundPort);
        }
        ~UdpSocket() override
        {
            if (m_handle != core::kInvalidSocket)
            {
                core::SocketClose(m_handle);
            }
            core::ShutdownNetworking();
        }
        UdpSocket(const UdpSocket&) = delete;
        UdpSocket& operator=(const UdpSocket&) = delete;

        [[nodiscard]] bool IsOpen() const noexcept { return m_handle != core::kInvalidSocket; }
        [[nodiscard]] u16 BoundPort() const noexcept { return m_boundPort; }

        void Send(const DatagramEndpoint& to, Span<const byte> data) override
        {
            if (m_handle == core::kInvalidSocket)
            {
                return;
            }
            (void)core::UdpSendTo(m_handle, EndpointIp(to), EndpointPort(to), data.Data(),
                                  data.Size());
        }

        [[nodiscard]] bool Receive(DatagramEndpoint& from, Array<byte>& out) override
        {
            if (m_handle == core::kInvalidSocket)
            {
                return false;
            }
            out.Resize(kMaxDatagram);
            u32 ip = 0;
            u16 port = 0;
            const i64 n = core::UdpRecvFrom(m_handle, out.Data(), out.Size(), ip, port);
            if (n <= 0)
            {
                out.Clear();
                return false;
            }
            out.Resize(static_cast<usize>(n));
            from = MakeEndpoint(ip, port);
            return true;
        }

        // For a real socket, the local endpoint is loopback and the bound port: the address a peer on
        // the same host connects to. A real remote uses the public or LAN ip; resolve it with
        // ResolveEndpoint.
        [[nodiscard]] DatagramEndpoint LocalEndpoint() const override
        {
            u32 loopback = 0;
            (void)core::ParseIPv4(u8"127.0.0.1", loopback);
            return MakeEndpoint(loopback, m_boundPort);
        }

    private:
        static constexpr usize kMaxDatagram = 2048; // > the reliable transport's maxPacketBytes
        core::SocketHandle m_handle = core::kInvalidSocket;
        u16 m_boundPort = 0;
    };

}
