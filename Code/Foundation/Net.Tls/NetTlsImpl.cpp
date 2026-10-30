// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.net.tls - implementation: the system's trusted roots, and the plain and TLS streams.

module;
#include "Core/Prelude.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#endif

module foundation.net.tls;

import foundation.core;
import foundation.net;

using namespace foundation::core;

namespace foundation::net
{
    TlsRoots LoadSystemRoots(String* where)
    {
        TlsRoots roots;
#if defined(_WIN32)
        // The machine's trusted roots, as Windows' own TLS checks them.
        HCERTSTORE store = ::CertOpenSystemStoreW(0, L"ROOT");
        if (store == nullptr)
        {
            if (where != nullptr)
                *where = String(u8"the Windows ROOT store did not open");
            return roots;
        }
        for (PCCERT_CONTEXT cert = ::CertEnumCertificatesInStore(store, nullptr); cert != nullptr;
             cert = ::CertEnumCertificatesInStore(store, cert))
        {
            if (cert->dwCertEncodingType != X509_ASN_ENCODING)
                continue;
            Array<byte> der;
            der.Resize(cert->cbCertEncoded);
            for (DWORD i = 0; i < cert->cbCertEncoded; ++i)
                der[i] = static_cast<byte>(cert->pbCertEncoded[i]);
            roots.der.PushBack(Move(der));
        }
        ::CertCloseStore(store, 0);
        if (where != nullptr)
            *where = Format(u8"the Windows ROOT store ({} certificates)", roots.der.Size());
#elif PLATFORM_WEB
        // A browser checks certificates itself; a web build has no TLS backend.
        if (where != nullptr)
            *where = String(u8"none: the browser does TLS");
#else
        // The bundle a distribution keeps (Debian/Ubuntu/SteamOS/Arch, Fedora/RHEL, openSUSE,
        // Alpine), the first that exists.
        static constexpr StringView kBundles[] = {
            u8"/etc/ssl/certs/ca-certificates.crt",
            u8"/etc/pki/tls/certs/ca-bundle.crt",
            u8"/etc/ssl/ca-bundle.pem",
            u8"/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
            u8"/etc/ssl/cert.pem",
        };
        for (StringView path : kBundles)
        {
            Result<Array<byte>> bytes = ReadFile(path);
            if (!bytes.HasValue() || bytes.Value().IsEmpty())
                continue;
            roots.pem = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            if (where != nullptr)
                *where = String(path);
            return roots;
        }
        if (where != nullptr)
            *where = String(u8"no bundle at /etc/ssl/certs/ca-certificates.crt or the other usual paths");
#endif
        return roots;
    }

    namespace
    {
        // As much of `pending` as the socket takes now, the rest kept; false when the socket is gone.
        bool SendSome(TcpSocket& socket, Array<byte>& pending)
        {
            usize sent = 0;
            while (sent < pending.Size())
            {
                const i64 n = socket.Send(Span<const byte>(pending.Data() + sent, pending.Size() - sent));
                if (n < 0)
                    return false;
                if (n == 0)
                    break; // would block: the rest on the next Pump
                sent += static_cast<usize>(n);
            }
            if (sent == pending.Size())
                pending.Clear();
            else if (sent > 0)
            {
                Array<byte> rest;
                for (usize i = sent; i < pending.Size(); ++i)
                    rest.PushBack(pending[i]);
                pending = Move(rest);
            }
            return true;
        }

        // The socket's connect, polled: 1 connected, 0 still connecting, -1 failed.
        int Connecting(TcpSocket& socket, bool& connected)
        {
            if (connected)
                return 1;
            if (!socket.IsOpen())
                return -1;
            const int status = socket.ConnectStatus();
            if (status > 0)
                connected = true;
            return status;
        }
    }

    // ---- PlainStream ---------------------------------------------------------------------

    bool PlainStream::Pump()
    {
        if (m_done)
            return false;
        const int status = Connecting(m_socket, m_connected);
        if (status < 0)
        {
            m_done = true;
            m_failure = String(u8"the connection could not be made");
            return false;
        }
        if (status == 0)
            return true;
        if (!SendSome(m_socket, m_out))
        {
            m_done = true;
            m_failure = String(u8"the connection closed while sending");
        }
        return !m_done;
    }

    i64 PlainStream::Send(Span<const byte> data)
    {
        if (m_done)
            return -1;
        for (const byte b : data)
            m_out.PushBack(b);
        if (m_connected && !SendSome(m_socket, m_out))
        {
            m_done = true;
            m_failure = String(u8"the connection closed while sending");
            return -1;
        }
        return static_cast<i64>(data.Size());
    }

    i64 PlainStream::Receive(Span<byte> out)
    {
        if (m_done)
            return -1;
        if (!m_connected)
            return 0;
        const i64 n = m_socket.Receive(out);
        if (n < 0)
            m_done = true; // the peer closed (or the connection broke)
        return n;
    }

    void PlainStream::Close()
    {
        (void)SendSome(m_socket, m_out);
        m_socket.Close();
        m_done = true;
    }

    // ---- TlsStream -----------------------------------------------------------------------

    bool TlsStream::Flush()
    {
        (void)m_session->TakeOutgoing(m_out);
        if (!m_connected || m_socketGone)
            return !m_socketGone;
        if (!SendSome(m_socket, m_out))
            m_socketGone = true;
        return !m_socketGone;
    }

    bool TlsStream::Pump()
    {
        if (m_session->State() == TlsState::Failed)
            return false;
        if (m_socketGone)
            return false;
        const int status = Connecting(m_socket, m_connected);
        if (status < 0)
        {
            m_socketGone = true;
            m_failure = String(u8"the connection could not be made");
            return false;
        }
        if (status == 0)
            return true;
        (void)Flush();
        byte buffer[16 * 1024];
        while (!m_socketGone)
        {
            const i64 n = m_socket.Receive(Span<byte>(buffer, sizeof(buffer)));
            if (n == 0)
                break;
            if (n < 0)
            {
                m_socketGone = true;
                break;
            }
            (void)m_session->Receive(Span<const byte>(buffer, static_cast<usize>(n)));
        }
        (void)Flush(); // the handshake's answers
        return m_session->State() != TlsState::Failed && m_session->State() != TlsState::Closed && !m_socketGone;
    }

    bool TlsStream::Ready() const { return m_connected && !m_socketGone && m_session->State() == TlsState::Open; }

    i64 TlsStream::Send(Span<const byte> data)
    {
        const TlsState state = m_session->State();
        if (m_socketGone || state == TlsState::Failed || state == TlsState::Closed)
            return -1;
        if (!m_session->Write(data).IsOk())
            return -1;
        (void)Flush();
        return static_cast<i64>(data.Size());
    }

    i64 TlsStream::Receive(Span<byte> out)
    {
        const usize n = m_session->Read(out);
        if (n > 0)
            return static_cast<i64>(n);
        const TlsState state = m_session->State();
        if (state == TlsState::Failed || state == TlsState::Closed || m_socketGone)
            return -1;
        return 0;
    }

    void TlsStream::Close()
    {
        m_session->Close();
        (void)Flush(); // the close_notify
        m_socket.Close();
        m_socketGone = true;
    }

    StringView TlsStream::Failure() const { return !m_failure.IsEmpty() ? m_failure.AsView() : m_session->Failure(); }
}
