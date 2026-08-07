/// Draconic::Net - `draconic.net:udp_socket` partition.
///
/// The REAL UDP IDatagramSocket backend: wraps the Foundation/System UDP primitives (docs/design/
/// networking.md §3.1 - sockets live in Foundation/System) so ReliableTransport, proven against the
/// deterministic sim, runs over an actual network with zero protocol changes. IPv4 for v1; a
/// DatagramEndpoint packs (ip << 16) | port in host order.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.net:udp_socket;

import draconic.foundation;
import :datagram;

using namespace draconic::foundation;

export namespace draconic::net
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

    // Parse "a.b.c.d" + port into an endpoint; an invalid (value 0) endpoint on parse failure.
    [[nodiscard]] inline DatagramEndpoint ResolveEndpoint(StringView dottedQuad, u16 port)
    {
        u32 ip = 0;
        if (!foundation::ParseIPv4(dottedQuad, ip))
        {
            return DatagramEndpoint{};
        }
        return MakeEndpoint(ip, port);
    }

    // A real UDP socket presented as an IDatagramSocket. Non-blocking; owns one Foundation/System handle and
    // a refcount on the platform networking layer.
    class UdpSocket final : public IDatagramSocket
    {
    public:
        // Bind to `port` (0 = OS-assigned). Check IsOpen() afterwards.
        explicit UdpSocket(u16 port = 0)
        {
            foundation::InitializeNetworking();
            m_handle = foundation::UdpOpen(port, &m_boundPort);
        }
        ~UdpSocket() override
        {
            if (m_handle != foundation::kInvalidSocket)
            {
                foundation::SocketClose(m_handle);
            }
            foundation::ShutdownNetworking();
        }
        UdpSocket(const UdpSocket&) = delete;
        UdpSocket& operator=(const UdpSocket&) = delete;

        [[nodiscard]] bool IsOpen() const noexcept { return m_handle != foundation::kInvalidSocket; }
        [[nodiscard]] u16 BoundPort() const noexcept { return m_boundPort; }

        void Send(const DatagramEndpoint& to, Span<const byte> data) override
        {
            if (m_handle == foundation::kInvalidSocket)
            {
                return;
            }
            (void)foundation::UdpSendTo(m_handle, EndpointIp(to), EndpointPort(to), data.Data(),
                                  data.Size());
        }

        [[nodiscard]] bool Receive(DatagramEndpoint& from, Array<byte>& out) override
        {
            if (m_handle == foundation::kInvalidSocket)
            {
                return false;
            }
            out.Resize(kMaxDatagram);
            u32 ip = 0;
            u16 port = 0;
            const i64 n = foundation::UdpRecvFrom(m_handle, out.Data(), out.Size(), ip, port);
            if (n <= 0)
            {
                out.Clear();
                return false;
            }
            out.Resize(static_cast<usize>(n));
            from = MakeEndpoint(ip, port);
            return true;
        }

        // For a real socket, "local endpoint" = loopback + the bound port - the address a peer on the
        // same host connects to. (A real remote uses the public/LAN ip; resolve it via ResolveEndpoint.)
        [[nodiscard]] DatagramEndpoint LocalEndpoint() const override
        {
            u32 loopback = 0;
            (void)foundation::ParseIPv4(u8"127.0.0.1", loopback);
            return MakeEndpoint(loopback, m_boundPort);
        }

    private:
        static constexpr usize kMaxDatagram = 2048; // > the reliable transport's maxPacketBytes
        foundation::SocketHandle m_handle = foundation::kInvalidSocket;
        u16 m_boundPort = 0;
    };

}
