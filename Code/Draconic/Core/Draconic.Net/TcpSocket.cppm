/// Draconic::Net - `draconic.net:tcp_socket` partition.
///
/// RAII TCP stream sockets over the Foundation/System TCP primitive (docs/design/networking.md §3.1), for
/// the future draconic.http / WebSocket / script-debugger transports - NOT the UDP game transport.
/// Each socket holds one WSA refcount (a no-op on POSIX), transferred on move. IPv4 for v1.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.net:tcp_socket;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::net
{

    // A connected TCP stream (a client connection, or one accepted by a TcpListener). Movable, RAII.
    class TcpSocket
    {
    public:
        TcpSocket() = default;
        // Adopt a handle; `ownsNet` = this instance holds a networking refcount to release on close.
        TcpSocket(foundation::SocketHandle handle, bool ownsNet) noexcept
            : m_handle(handle), m_ownsNet(ownsNet)
        {
        }
        ~TcpSocket() { Close(); }
        TcpSocket(TcpSocket&& o) noexcept : m_handle(o.m_handle), m_ownsNet(o.m_ownsNet)
        {
            o.m_handle = foundation::kInvalidSocket;
            o.m_ownsNet = false;
        }
        TcpSocket& operator=(TcpSocket&& o) noexcept
        {
            if (this != &o)
            {
                Close();
                m_handle = o.m_handle;
                m_ownsNet = o.m_ownsNet;
                o.m_handle = foundation::kInvalidSocket;
                o.m_ownsNet = false;
            }
            return *this;
        }
        TcpSocket(const TcpSocket&) = delete;
        TcpSocket& operator=(const TcpSocket&) = delete;

        // Begin a NON-BLOCKING connect to (dottedQuad, port). Poll ConnectStatus() until 1 (connected)
        // or -1 (failed). !IsOpen() if the address was unparseable.
        [[nodiscard]] static TcpSocket Connect(StringView dottedQuad, u16 port)
        {
            foundation::InitializeNetworking();
            u32 ip = 0;
            if (!foundation::ParseIPv4(dottedQuad, ip))
            {
                foundation::ShutdownNetworking();
                return TcpSocket{};
            }
            return TcpSocket(foundation::TcpConnect(ip, port), /*ownsNet=*/true);
        }

        [[nodiscard]] bool IsOpen() const noexcept { return m_handle != foundation::kInvalidSocket; }
        // 1 = connected, 0 = still connecting, -1 = failed.
        [[nodiscard]] int ConnectStatus() const noexcept
        {
            return foundation::TcpConnectStatus(m_handle);
        }
        // Bytes sent (may be partial), 0 = would-block (retry), -1 = closed/error.
        [[nodiscard]] i64 Send(Span<const byte> data) noexcept
        {
            return foundation::TcpSend(m_handle, data.Data(), data.Size());
        }
        // Bytes read (>0), 0 = would-block (no data yet), -1 = closed/error.
        [[nodiscard]] i64 Receive(Span<byte> out) noexcept
        {
            return foundation::TcpRecv(m_handle, out.Data(), out.Size());
        }
        void Close() noexcept
        {
            if (m_handle != foundation::kInvalidSocket)
            {
                foundation::SocketClose(m_handle);
                m_handle = foundation::kInvalidSocket;
            }
            if (m_ownsNet)
            {
                foundation::ShutdownNetworking();
                m_ownsNet = false;
            }
        }
        [[nodiscard]] foundation::SocketHandle Handle() const noexcept { return m_handle; }

    private:
        foundation::SocketHandle m_handle = foundation::kInvalidSocket;
        bool m_ownsNet = false;
    };

    // A TCP listening socket; Accept() returns pending client connections (non-blocking).
    class TcpListener
    {
    public:
        explicit TcpListener(u16 port = 0)
        {
            foundation::InitializeNetworking();
            m_handle = foundation::TcpListen(port, &m_boundPort);
        }
        ~TcpListener()
        {
            if (m_handle != foundation::kInvalidSocket)
            {
                foundation::SocketClose(m_handle);
            }
            foundation::ShutdownNetworking();
        }
        TcpListener(const TcpListener&) = delete;
        TcpListener& operator=(const TcpListener&) = delete;

        [[nodiscard]] bool IsOpen() const noexcept { return m_handle != foundation::kInvalidSocket; }
        [[nodiscard]] u16 BoundPort() const noexcept { return m_boundPort; }
        // Accept one pending connection; the returned socket is !IsOpen() when none is pending.
        [[nodiscard]] TcpSocket Accept()
        {
            const foundation::SocketHandle h = foundation::TcpAccept(m_handle, nullptr, nullptr);
            if (h == foundation::kInvalidSocket)
            {
                return TcpSocket{};
            }
            foundation::InitializeNetworking(); // independent refcount for the accepted socket
            return TcpSocket(h, /*ownsNet=*/true);
        }

    private:
        foundation::SocketHandle m_handle = foundation::kInvalidSocket;
        u16 m_boundPort = 0;
    };

}
